#include "remedy/ports/channel_port.h"
#include "remedy/wire_frame.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <stdio.h>

enum channel_token_state {
    CHANNEL_TOKEN_LIVE    = 1,
    CHANNEL_TOKEN_CLOSING = 2,
    CHANNEL_TOKEN_RETIRED = 3
};

struct win32_channel_entry {
    HANDLE pipe_handle{INVALID_HANDLE_VALUE};
    bool   is_server{false};
    std::atomic<uint32_t> state{CHANNEL_TOKEN_LIVE};
    std::atomic<uint32_t> op_count{0};
};

class channel_op_lease {
public:
    explicit channel_op_lease(win32_channel_entry* entry) : entry_(entry) {
        if (entry_) {
            entry_->op_count.fetch_add(1, std::memory_order_relaxed);
            if (entry_->state.load(std::memory_order_acquire) != CHANNEL_TOKEN_LIVE) {
                entry_->op_count.fetch_sub(1, std::memory_order_release);
                entry_ = nullptr;
            }
        }
    }

    ~channel_op_lease() {
        if (entry_) {
            entry_->op_count.fetch_sub(1, std::memory_order_release);
        }
    }

    explicit operator bool() const { return entry_ != nullptr; }
    win32_channel_entry* get() const { return entry_; }

private:
    win32_channel_entry* entry_{nullptr};
};

static std::mutex g_channel_mutex;
static std::unordered_map<remedy_channel_token_t, win32_channel_entry*> g_channel_table;
static std::atomic<uint64_t> g_next_channel_token{1};

static remedy_err_t read_exact(HANDLE hPipe, uint8_t* buffer, size_t count) {
    size_t total = 0;
    while (total < count) {
        DWORD readBytes = 0;
        BOOL ok = ReadFile(hPipe, buffer + total, (DWORD)(count - total), &readBytes, NULL);
        if (!ok || readBytes == 0) return REMEDY_ERR_IPC_FAILURE;
        total += readBytes;
    }
    return REMEDY_OK;
}

static remedy_err_t write_exact(HANDLE hPipe, const uint8_t* buffer, size_t count) {
    size_t total = 0;
    while (total < count) {
        DWORD written = 0;
        BOOL ok = WriteFile(hPipe, buffer + total, (DWORD)(count - total), &written, NULL);
        if (!ok || written == 0) return REMEDY_ERR_IPC_FAILURE;
        total += written;
    }
    return REMEDY_OK;
}

extern "C" {

remedy_err_t channel_port_create(const remedy_channel_config_t* config, remedy_channel_token_t* out_token) {
    if (!config || !out_token || !config->channel_name) return REMEDY_ERR_INVALID_ARGUMENT;

    char pipe_path[256];
    snprintf(pipe_path, sizeof(pipe_path), "\\\\.\\pipe\\remedy-worker-%s", config->channel_name);

    wchar_t wPath[256];
    MultiByteToWideChar(CP_UTF8, 0, pipe_path, -1, wPath, 256);

    HANDLE hPipe = INVALID_HANDLE_VALUE;
    if (config->is_server) {
        hPipe = CreateNamedPipeW(
            wPath,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, NULL
        );
    } else {
        hPipe = CreateFileW(
            wPath,
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, NULL
        );
    }

    if (hPipe == INVALID_HANDLE_VALUE) return REMEDY_ERR_IPC_FAILURE;

    win32_channel_entry* entry = new win32_channel_entry();
    entry->pipe_handle = hPipe;
    entry->is_server = config->is_server;

    remedy_channel_token_t token = g_next_channel_token.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        g_channel_table[token] = entry;
    }

    *out_token = token;
    return REMEDY_OK;
}

remedy_err_t channel_port_connect(remedy_channel_token_t token, uint32_t timeout_ms) {
    win32_channel_entry* raw_entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        auto it = g_channel_table.find(token);
        if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        raw_entry = it->second;
    }

    channel_op_lease lease(raw_entry);
    if (!lease) return REMEDY_ERR_HANDLE_STALE;
    win32_channel_entry* entry = lease.get();

    if (entry->is_server) {
        OVERLAPPED ov = { 0 };
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) return REMEDY_ERR_IPC_FAILURE;

        BOOL conn = ConnectNamedPipe(entry->pipe_handle, &ov);
        DWORD err = GetLastError();

        if (conn || err == ERROR_PIPE_CONNECTED) {
            CloseHandle(ov.hEvent);
            return REMEDY_OK;
        }

        if (err == ERROR_IO_PENDING) {
            DWORD waitRes = WaitForSingleObject(ov.hEvent, timeout_ms);
            if (waitRes == WAIT_OBJECT_0) {
                DWORD bytesTransferred = 0;
                BOOL getRes = GetOverlappedResult(entry->pipe_handle, &ov, &bytesTransferred, FALSE);
                CloseHandle(ov.hEvent);
                return getRes ? REMEDY_OK : REMEDY_ERR_IPC_FAILURE;
            } else {
                CancelIoEx(entry->pipe_handle, &ov);
                CloseHandle(ov.hEvent);
                return REMEDY_ERR_TIMEOUT;
            }
        }

        CloseHandle(ov.hEvent);
        return REMEDY_ERR_IPC_FAILURE;
    }
    return REMEDY_OK;
}

remedy_err_t channel_port_send_frame(remedy_channel_token_t token, const remedy_wire_frame_header_t* header, const void* payload) {
    win32_channel_entry* raw_entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        auto it = g_channel_table.find(token);
        if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        raw_entry = it->second;
    }

    channel_op_lease lease(raw_entry);
    if (!lease) return REMEDY_ERR_HANDLE_STALE;
    win32_channel_entry* entry = lease.get();

    if (!header || entry->pipe_handle == INVALID_HANDLE_VALUE) return REMEDY_ERR_INVALID_ARGUMENT;

    remedy_wire_frame_header_t local_hdr = *header;
    local_hdr.magic = REMEDY_WIRE_MAGIC;
    local_hdr.version = REMEDY_WIRE_VERSION;
    local_hdr.header_len = REMEDY_WIRE_HEADER_SIZE;

    if (payload && local_hdr.payload_len > 0) {
        local_hdr.checksum = remedy_adler32((const uint8_t*)payload, local_hdr.payload_len);
    } else {
        local_hdr.payload_len = 0;
        local_hdr.checksum = 0;
    }

    uint8_t header_buf[36];
    remedy_wire_frame_encode(&local_hdr, header_buf);

    remedy_err_t w_hdr_err = write_exact(entry->pipe_handle, header_buf, 36);
    if (w_hdr_err != REMEDY_OK) return w_hdr_err;

    if (payload && local_hdr.payload_len > 0) {
        remedy_err_t w_pay_err = write_exact(entry->pipe_handle, (const uint8_t*)payload, local_hdr.payload_len);
        if (w_pay_err != REMEDY_OK) return w_pay_err;
    }

    return REMEDY_OK;
}

remedy_err_t channel_port_read_frame(remedy_channel_token_t token, remedy_wire_frame_header_t* out_header, void* payload_buffer, size_t max_payload_len) {
    win32_channel_entry* raw_entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        auto it = g_channel_table.find(token);
        if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        raw_entry = it->second;
    }

    channel_op_lease lease(raw_entry);
    if (!lease) return REMEDY_ERR_HANDLE_STALE;
    win32_channel_entry* entry = lease.get();

    if (!out_header || entry->pipe_handle == INVALID_HANDLE_VALUE) return REMEDY_ERR_INVALID_ARGUMENT;

    uint8_t header_buf[36];
    remedy_err_t r_hdr_err = read_exact(entry->pipe_handle, header_buf, 36);
    if (r_hdr_err != REMEDY_OK) return r_hdr_err;

    remedy_err_t dec_err = remedy_wire_frame_decode(header_buf, out_header);
    if (dec_err != REMEDY_OK) return dec_err;

    if (out_header->payload_len > 0) {
        if (out_header->payload_len > max_payload_len || !payload_buffer) {
            return REMEDY_ERR_INVALID_ARGUMENT;
        }

        remedy_err_t r_pay_err = read_exact(entry->pipe_handle, (uint8_t*)payload_buffer, out_header->payload_len);
        if (r_pay_err != REMEDY_OK) return r_pay_err;

        uint32_t actual_chk = remedy_adler32((const uint8_t*)payload_buffer, out_header->payload_len);
        if (actual_chk != out_header->checksum) return REMEDY_ERR_IPC_FAILURE;
    }

    return REMEDY_OK;
}

remedy_err_t channel_port_close(remedy_channel_token_t token) {
    win32_channel_entry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        auto it = g_channel_table.find(token);
        if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        entry = it->second;
    }

    if (entry->pipe_handle != INVALID_HANDLE_VALUE) {
        CancelIoEx(entry->pipe_handle, NULL);
    }
    return REMEDY_OK;
}

remedy_err_t channel_port_destroy(remedy_channel_token_t token) {
    win32_channel_entry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_channel_mutex);
        auto it = g_channel_table.find(token);
        if (it == g_channel_table.end()) return REMEDY_ERR_INVALID_ARGUMENT;
        entry = it->second;
        g_channel_table.erase(it);
    }

    // Mark CLOSING
    entry->state.store(CHANNEL_TOKEN_CLOSING, std::memory_order_release);

    // Cancel pending I/O
    if (entry->pipe_handle != INVALID_HANDLE_VALUE) {
        CancelIoEx(entry->pipe_handle, NULL);
    }

    // Wait for active operation pins to drain
    while (entry->op_count.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    if (entry->pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(entry->pipe_handle);
        entry->pipe_handle = INVALID_HANDLE_VALUE;
    }

    entry->state.store(CHANNEL_TOKEN_RETIRED, std::memory_order_release);
    delete entry;
    return REMEDY_OK;
}

} // extern "C"
