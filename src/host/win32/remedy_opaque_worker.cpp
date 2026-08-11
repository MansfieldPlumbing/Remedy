#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "remedy/wire_frame.h"

static constexpr uint32_t REMEDY_OPAQUE_PAYLOAD_LIMIT = 256;

static bool read_exact(HANDLE endpoint, void* buffer, DWORD length) {
    auto* cursor = static_cast<uint8_t*>(buffer);
    DWORD remaining = length;
    while (remaining != 0) {
        DWORD received = 0;
        if (!ReadFile(endpoint, cursor, remaining, &received, nullptr) || received == 0) return false;
        cursor += received;
        remaining -= received;
    }
    return true;
}

static bool write_exact(HANDLE endpoint, const void* buffer, DWORD length) {
    const auto* cursor = static_cast<const uint8_t*>(buffer);
    DWORD remaining = length;
    while (remaining != 0) {
        DWORD sent = 0;
        if (!WriteFile(endpoint, cursor, remaining, &sent, nullptr) || sent == 0) return false;
        cursor += sent;
        remaining -= sent;
    }
    return true;
}

static bool write_frame(
    HANDLE endpoint,
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

int main(int argc, char** argv) {
    static const char prefix[] = "--remedy-channel-handle=";
    if (argc != 2 || strncmp(argv[1], prefix, sizeof(prefix) - 1) != 0) return 2;

    char* end = nullptr;
    const unsigned long long raw = _strtoui64(argv[1] + sizeof(prefix) - 1, &end, 10);
    if (raw == 0 || end == nullptr || *end != '\0') return 2;

    HANDLE endpoint = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
    DWORD flags = 0;
    if (!GetHandleInformation(endpoint, &flags) || GetFileType(endpoint) != FILE_TYPE_PIPE) return 3;

    if (!write_frame(endpoint, REMEDY_WIRE_KIND_READY, 0, 0, nullptr, 0)) return 4;

    uint8_t encoded[REMEDY_WIRE_HEADER_SIZE]{};
    remedy_wire_frame_header_t request{};
    if (!read_exact(endpoint, encoded, sizeof(encoded))) return 5;
    if (remedy_wire_frame_decode(encoded, &request) != REMEDY_OK) return 6;
    if (request.kind != REMEDY_WIRE_KIND_REQUEST ||
        request.payload_len > REMEDY_OPAQUE_PAYLOAD_LIMIT) return 7;

    uint8_t payload[REMEDY_OPAQUE_PAYLOAD_LIMIT]{};
    if (request.payload_len != 0 && !read_exact(endpoint, payload, request.payload_len)) return 8;
    const uint32_t expected_checksum = request.payload_len == 0 ? 0 : remedy_adler32(payload, request.payload_len);
    if (request.checksum != expected_checksum) return 9;
    for (uint32_t i = 0; i < request.payload_len; ++i) payload[i] ^= 0x5aU;

    if (!write_frame(
            endpoint,
            REMEDY_WIRE_KIND_COMPLETION,
            request.request_id,
            request.domain_handle,
            payload,
            request.payload_len)) return 10;

    remedy_wire_frame_header_t quiesce{};
    if (!read_exact(endpoint, encoded, sizeof(encoded))) return 11;
    if (remedy_wire_frame_decode(encoded, &quiesce) != REMEDY_OK) return 12;
    if (quiesce.kind != REMEDY_WIRE_KIND_QUIESCE ||
        quiesce.payload_len != 0 ||
        quiesce.checksum != 0 ||
        quiesce.request_id != request.request_id ||
        quiesce.domain_handle != request.domain_handle) return 13;

    if (!write_frame(
            endpoint,
            REMEDY_WIRE_KIND_QUIESCE_ACK,
            quiesce.request_id,
            quiesce.domain_handle,
            nullptr,
            0)) return 14;
    if (!FlushFileBuffers(endpoint)) return 15;
    if (!CloseHandle(endpoint)) return 16;
    return 0;
}
