#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdint.h>

#include "remedy/wire_frame.h"

static constexpr int REMEDY_WORKER_CHANNEL_FD = 3;

static bool read_exact(int fd, void* buffer, size_t length) {
    auto* cursor = static_cast<uint8_t*>(buffer);
    size_t remaining = length;
    while (remaining != 0) {
        ssize_t received = read(fd, cursor, remaining);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return false;
        cursor += received;
        remaining -= static_cast<size_t>(received);
    }
    return true;
}

static bool write_exact(int fd, const void* buffer, size_t length) {
    const auto* cursor = static_cast<const uint8_t*>(buffer);
    size_t remaining = length;
    while (remaining != 0) {
        ssize_t sent = write(fd, cursor, remaining);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        cursor += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

int main(int argc, char**) {
    if (argc != 1) return 2;
    struct stat endpoint_stat{};
    if (fstat(REMEDY_WORKER_CHANNEL_FD, &endpoint_stat) != 0 || !S_ISSOCK(endpoint_stat.st_mode)) return 3;
    for (int fd = 4; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) return 12;
    }

    uint8_t encoded[REMEDY_WIRE_HEADER_SIZE]{};
    remedy_wire_frame_header_t request{};
    if (!read_exact(REMEDY_WORKER_CHANNEL_FD, encoded, sizeof(encoded))) return 4;
    if (remedy_wire_frame_decode(encoded, &request) != REMEDY_OK) return 5;
    if (request.kind != REMEDY_WIRE_KIND_PING || request.payload_len != 0 || request.checksum != 0) return 6;

    remedy_wire_frame_header_t response{};
    response.magic = REMEDY_WIRE_MAGIC;
    response.version = REMEDY_WIRE_VERSION;
    response.kind = REMEDY_WIRE_KIND_PONG;
    response.header_len = REMEDY_WIRE_HEADER_SIZE;
    response.request_id = request.request_id;
    response.domain_handle = request.domain_handle;
    remedy_wire_frame_encode(&response, encoded);
    if (!write_exact(REMEDY_WORKER_CHANNEL_FD, encoded, sizeof(encoded))) return 7;

    remedy_wire_frame_header_t release{};
    if (!read_exact(REMEDY_WORKER_CHANNEL_FD, encoded, sizeof(encoded))) return 8;
    if (remedy_wire_frame_decode(encoded, &release) != REMEDY_OK) return 9;
    if (release.kind != REMEDY_WIRE_KIND_QUIESCE || release.payload_len != 0) return 10;
    if (close(REMEDY_WORKER_CHANNEL_FD) != 0) return 11;
    return 0;
}
