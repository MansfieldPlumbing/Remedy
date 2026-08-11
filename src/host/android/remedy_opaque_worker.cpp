#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdint.h>

#include "remedy/wire_frame.h"

static constexpr int REMEDY_WORKER_CHANNEL_FD = 3;
static constexpr uint32_t REMEDY_OPAQUE_PAYLOAD_LIMIT = 256;

static bool read_exact(int endpoint, void* buffer, size_t length) {
    auto* cursor = static_cast<uint8_t*>(buffer);
    size_t remaining = length;
    while (remaining != 0) {
        const ssize_t received = read(endpoint, cursor, remaining);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        cursor += received;
        remaining -= static_cast<size_t>(received);
    }
    return true;
}

static bool write_exact(int endpoint, const void* buffer, size_t length) {
    const auto* cursor = static_cast<const uint8_t*>(buffer);
    size_t remaining = length;
    while (remaining != 0) {
        const ssize_t sent = write(endpoint, cursor, remaining);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        cursor += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

static bool write_frame(
    int endpoint,
    uint16_t kind,
    uint64_t correlation,
    uint64_t generation,
    const uint8_t* payload,
    uint32_t payload_length) {
    remedy_wire_frame_header_t header{};
    header.magic = REMEDY_WIRE_MAGIC;
    header.version = REMEDY_WIRE_VERSION;
    header.kind = kind;
    header.header_len = REMEDY_WIRE_HEADER_SIZE;
    header.payload_len = payload_length;
    header.request_id = correlation;
    header.domain_handle = generation;
    header.checksum = payload_length == 0 ? 0 : remedy_adler32(payload, payload_length);

    uint8_t encoded[REMEDY_WIRE_HEADER_SIZE]{};
    remedy_wire_frame_encode(&header, encoded);
    if (!write_exact(endpoint, encoded, sizeof(encoded))) return false;
    return payload_length == 0 || write_exact(endpoint, payload, payload_length);
}

int main(int argc, char**) {
    if (argc != 1) return 2;

    struct stat endpoint_stat{};
    if (fstat(REMEDY_WORKER_CHANNEL_FD, &endpoint_stat) != 0 ||
        !S_ISSOCK(endpoint_stat.st_mode)) return 3;

    if (!write_frame(REMEDY_WORKER_CHANNEL_FD, REMEDY_WIRE_KIND_READY, 0, 0, nullptr, 0)) return 4;

    uint8_t encoded[REMEDY_WIRE_HEADER_SIZE]{};
    remedy_wire_frame_header_t request{};
    if (!read_exact(REMEDY_WORKER_CHANNEL_FD, encoded, sizeof(encoded))) return 5;
    if (remedy_wire_frame_decode(encoded, &request) != REMEDY_OK) return 6;
    if (request.kind != REMEDY_WIRE_KIND_REQUEST ||
        request.payload_len > REMEDY_OPAQUE_PAYLOAD_LIMIT) return 7;

    uint8_t payload[REMEDY_OPAQUE_PAYLOAD_LIMIT]{};
    if (request.payload_len != 0 &&
        !read_exact(REMEDY_WORKER_CHANNEL_FD, payload, request.payload_len)) return 8;
    const uint32_t expected_checksum = request.payload_len == 0 ? 0 : remedy_adler32(payload, request.payload_len);
    if (request.checksum != expected_checksum) return 9;
    for (uint32_t i = 0; i < request.payload_len; ++i) payload[i] ^= 0x5aU;

    if (!write_frame(
            REMEDY_WORKER_CHANNEL_FD,
            REMEDY_WIRE_KIND_COMPLETION,
            request.request_id,
            request.domain_handle,
            payload,
            request.payload_len)) return 10;

    remedy_wire_frame_header_t quiesce{};
    if (!read_exact(REMEDY_WORKER_CHANNEL_FD, encoded, sizeof(encoded))) return 11;
    if (remedy_wire_frame_decode(encoded, &quiesce) != REMEDY_OK) return 12;
    if (quiesce.kind != REMEDY_WIRE_KIND_QUIESCE ||
        quiesce.payload_len != 0 ||
        quiesce.checksum != 0 ||
        quiesce.request_id != request.request_id ||
        quiesce.domain_handle != request.domain_handle) return 13;

    if (!write_frame(
            REMEDY_WORKER_CHANNEL_FD,
            REMEDY_WIRE_KIND_QUIESCE_ACK,
            quiesce.request_id,
            quiesce.domain_handle,
            nullptr,
            0)) return 14;
    if (close(REMEDY_WORKER_CHANNEL_FD) != 0) return 15;
    return 0;
}
