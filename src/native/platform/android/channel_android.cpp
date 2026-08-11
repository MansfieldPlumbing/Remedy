#include "remedy/ports/channel_port.h"
#include "channel_worker_android.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <mutex>
#include <new>
#include <unordered_map>

struct android_channel_entry {
    int executive_fd{-1};
    int worker_fd{-1};
    bool endpoint_issued{false};
    bool closed{false};
};

static std::mutex g_channel_mutex;
static std::unordered_map<remedy_channel_token_t, android_channel_entry*> g_channel_table;
static remedy_channel_token_t g_next_channel_token = 1;

static remedy_err_t transfer_exact(int fd, void* buffer, size_t length, bool write_operation) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    size_t completed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (completed < length) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return REMEDY_ERR_TIMEOUT;
        pollfd descriptor{fd, static_cast<short>(write_operation ? POLLOUT : POLLIN), 0};
        int poll_result = poll(&descriptor, 1, static_cast<int>(remaining));
        if (poll_result == 0) return REMEDY_ERR_TIMEOUT;
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            return REMEDY_ERR_IPC_FAILURE;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) return REMEDY_ERR_IPC_FAILURE;
        ssize_t result = write_operation
            ? write(fd, bytes + completed, length - completed)
            : read(fd, bytes + completed, length - completed);
        if (result == 0) return REMEDY_ERR_IPC_FAILURE;
        if (result < 0) {
            if (errno == EINTR) continue;
            return REMEDY_ERR_IPC_FAILURE;
        }
        completed += static_cast<size_t>(result);
    }
    return REMEDY_OK;
}

extern "C" remedy_err_t channel_port_create(
    const remedy_channel_config_t* config,
    remedy_channel_token_t* out_token) {
    if (!out_token) return REMEDY_ERR_INVALID_ARGUMENT;
    *out_token = REMEDY_INVALID_CHANNEL_TOKEN;
    if (!config || !config->is_server || !config->channel_name || config->channel_name[0] == '\0') {
        return REMEDY_ERR_INVALID_ARGUMENT;
    }

    int endpoints[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, endpoints) != 0) {
        return REMEDY_ERR_IPC_FAILURE;
    }
    auto* entry = new (std::nothrow) android_channel_entry();
    if (!entry) {
        bool first_closed = close(endpoints[0]) == 0;
        bool second_closed = close(endpoints[1]) == 0;
        return (first_closed && second_closed) ? REMEDY_ERR_OUT_OF_MEMORY : REMEDY_ERR_CONTAINMENT_FAILED;
    }
    entry->executive_fd = endpoints[0];
    entry->worker_fd = endpoints[1];

    std::lock_guard<std::mutex> lock(g_channel_mutex);
    remedy_channel_token_t token = g_next_channel_token++;
    if (token == REMEDY_INVALID_CHANNEL_TOKEN || g_channel_table.find(token) != g_channel_table.end()) {
        bool first_closed = close(entry->executive_fd) == 0;
        bool second_closed = close(entry->worker_fd) == 0;
        delete entry;
        return (first_closed && second_closed) ? REMEDY_ERR_OUT_OF_MEMORY : REMEDY_ERR_CONTAINMENT_FAILED;
    }
    try {
        auto insertion = g_channel_table.emplace(token, entry);
        if (!insertion.second) {
            bool first_closed = close(entry->executive_fd) == 0;
            bool second_closed = close(entry->worker_fd) == 0;
            delete entry;
            return (first_closed && second_closed) ? REMEDY_ERR_OUT_OF_MEMORY : REMEDY_ERR_CONTAINMENT_FAILED;
        }
    } catch (...) {
        bool first_closed = close(entry->executive_fd) == 0;
        bool second_closed = close(entry->worker_fd) == 0;
        delete entry;
        return (first_closed && second_closed) ? REMEDY_ERR_OUT_OF_MEMORY : REMEDY_ERR_CONTAINMENT_FAILED;
    }
    *out_token = token;
    return REMEDY_OK;
}

remedy_err_t channel_android_issue_worker_endpoint(
    remedy_channel_token_t token,
    int* out_worker_endpoint) {
    if (!out_worker_endpoint) return REMEDY_ERR_INVALID_ARGUMENT;
    *out_worker_endpoint = -1;
    std::lock_guard<std::mutex> lock(g_channel_mutex);
    auto it = g_channel_table.find(token);
    if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
    android_channel_entry* entry = it->second;
    if (entry->closed) return REMEDY_ERR_REVOKING;
    if (entry->endpoint_issued || entry->worker_fd < 0) return REMEDY_ERR_REVOKING;
    entry->endpoint_issued = true;
    *out_worker_endpoint = entry->worker_fd;
    entry->worker_fd = -1;
    return REMEDY_OK;
}

extern "C" remedy_err_t channel_port_connect(remedy_channel_token_t token, uint32_t) {
    std::lock_guard<std::mutex> lock(g_channel_mutex);
    auto it = g_channel_table.find(token);
    if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
    return it->second->closed ? REMEDY_ERR_REVOKING : REMEDY_OK;
}

extern "C" remedy_err_t channel_port_send_frame(
    remedy_channel_token_t token,
    const remedy_wire_frame_header_t* header,
    const void* payload) {
    if (!header || (header->payload_len != 0 && !payload)) return REMEDY_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_channel_mutex);
    auto it = g_channel_table.find(token);
    if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
    android_channel_entry* entry = it->second;
    if (entry->closed || entry->executive_fd < 0) return REMEDY_ERR_REVOKING;

    remedy_wire_frame_header_t local = *header;
    local.magic = REMEDY_WIRE_MAGIC;
    local.version = REMEDY_WIRE_VERSION;
    local.header_len = REMEDY_WIRE_HEADER_SIZE;
    local.checksum = local.payload_len == 0
        ? 0
        : remedy_adler32(static_cast<const uint8_t*>(payload), local.payload_len);
    uint8_t encoded[REMEDY_WIRE_HEADER_SIZE]{};
    remedy_wire_frame_encode(&local, encoded);
    remedy_err_t result = transfer_exact(entry->executive_fd, encoded, sizeof(encoded), true);
    if (result != REMEDY_OK || local.payload_len == 0) return result;
    return transfer_exact(
        entry->executive_fd,
        const_cast<void*>(payload),
        local.payload_len,
        true);
}

extern "C" remedy_err_t channel_port_read_frame(
    remedy_channel_token_t token,
    remedy_wire_frame_header_t* out_header,
    void* payload_buffer,
    size_t max_payload_len) {
    if (!out_header) return REMEDY_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_channel_mutex);
    auto it = g_channel_table.find(token);
    if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
    android_channel_entry* entry = it->second;
    if (entry->closed || entry->executive_fd < 0) return REMEDY_ERR_REVOKING;

    uint8_t encoded[REMEDY_WIRE_HEADER_SIZE]{};
    remedy_err_t result = transfer_exact(entry->executive_fd, encoded, sizeof(encoded), false);
    if (result != REMEDY_OK) return result;
    remedy_wire_frame_header_t header{};
    result = remedy_wire_frame_decode(encoded, &header);
    if (result != REMEDY_OK) return result;
    if (header.payload_len > max_payload_len || (header.payload_len != 0 && !payload_buffer)) {
        return REMEDY_ERR_INVALID_ARGUMENT;
    }
    if (header.payload_len != 0) {
        result = transfer_exact(entry->executive_fd, payload_buffer, header.payload_len, false);
        if (result != REMEDY_OK) return result;
        if (remedy_adler32(static_cast<const uint8_t*>(payload_buffer), header.payload_len) != header.checksum) {
            return REMEDY_ERR_IPC_FAILURE;
        }
    } else if (header.checksum != 0) {
        return REMEDY_ERR_IPC_FAILURE;
    }
    *out_header = header;
    return REMEDY_OK;
}

extern "C" remedy_err_t channel_port_close(remedy_channel_token_t token) {
    std::lock_guard<std::mutex> lock(g_channel_mutex);
    auto it = g_channel_table.find(token);
    if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
    android_channel_entry* entry = it->second;
    if (entry->closed) return REMEDY_OK;
    bool clean = true;
    if (entry->executive_fd >= 0) {
        clean = close(entry->executive_fd) == 0;
        entry->executive_fd = -1;
    }
    if (entry->worker_fd >= 0) {
        clean = (close(entry->worker_fd) == 0) && clean;
        entry->worker_fd = -1;
    }
    entry->closed = true;
    return clean ? REMEDY_OK : REMEDY_ERR_CONTAINMENT_FAILED;
}

extern "C" remedy_err_t channel_port_destroy(remedy_channel_token_t token) {
    std::lock_guard<std::mutex> lock(g_channel_mutex);
    auto it = g_channel_table.find(token);
    if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
    if (!it->second->closed) return REMEDY_ERR_INVALID_ARGUMENT;
    delete it->second;
    g_channel_table.erase(it);
    return REMEDY_OK;
}
