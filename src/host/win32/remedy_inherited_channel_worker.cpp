#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "remedy/wire_frame.h"

static bool read_exact(HANDLE handle, void* buffer, DWORD length) {
    uint8_t* cursor = static_cast<uint8_t*>(buffer);
    DWORD remaining = length;
    while (remaining != 0) {
        DWORD received = 0;
        if (!ReadFile(handle, cursor, remaining, &received, NULL) || received == 0) return false;
        cursor += received;
        remaining -= received;
    }
    return true;
}

static bool write_exact(HANDLE handle, const void* buffer, DWORD length) {
    const uint8_t* cursor = static_cast<const uint8_t*>(buffer);
    DWORD remaining = length;
    while (remaining != 0) {
        DWORD sent = 0;
        if (!WriteFile(handle, cursor, remaining, &sent, NULL) || sent == 0) return false;
        cursor += sent;
        remaining -= sent;
    }
    return true;
}

static bool inheritance_canaries_absent() {
    char canaries[2048]{};
    DWORD length = GetEnvironmentVariableA("REMEDY_TEST_CANARY_HANDLES", canaries, sizeof(canaries));
    if (length == 0) return GetLastError() == ERROR_ENVVAR_NOT_FOUND;
    if (length >= sizeof(canaries)) return false;

    char* cursor = canaries;
    while (*cursor != '\0') {
        char* end = nullptr;
        unsigned long long raw = _strtoui64(cursor, &end, 10);
        if (raw == 0 || end == cursor || (*end != ',' && *end != '\0')) return false;
        HANDLE candidate = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
        if (SetEvent(candidate)) return false;
        cursor = (*end == ',') ? end + 1 : end;
    }
    return true;
}

int main(int argc, char** argv) {
    static const char prefix[] = "--remedy-channel-handle=";
    if (argc != 2 || strncmp(argv[1], prefix, sizeof(prefix) - 1) != 0) return 2;

    char* end = nullptr;
    unsigned long long raw = _strtoui64(argv[1] + sizeof(prefix) - 1, &end, 10);
    if (raw == 0 || !end || *end != '\0') return 2;

    HANDLE endpoint = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw));
    DWORD flags = 0;
    if (!GetHandleInformation(endpoint, &flags)) return 3;
    if (GetFileType(endpoint) != FILE_TYPE_PIPE) return 3;
    if (!inheritance_canaries_absent()) return 12;

    uint8_t encoded[REMEDY_WIRE_HEADER_SIZE]{};
    remedy_wire_frame_header_t request{};
    if (!read_exact(endpoint, encoded, REMEDY_WIRE_HEADER_SIZE)) return 4;
    if (remedy_wire_frame_decode(encoded, &request) != REMEDY_OK) return 5;
    if (request.kind != REMEDY_WIRE_KIND_PING || request.payload_len != 0) return 6;

    remedy_wire_frame_header_t response{};
    response.magic = REMEDY_WIRE_MAGIC;
    response.version = REMEDY_WIRE_VERSION;
    response.kind = REMEDY_WIRE_KIND_PONG;
    response.header_len = REMEDY_WIRE_HEADER_SIZE;
    response.request_id = request.request_id;
    response.domain_handle = request.domain_handle;
    response.checksum = 0;
    remedy_wire_frame_encode(&response, encoded);

    if (!write_exact(endpoint, encoded, REMEDY_WIRE_HEADER_SIZE)) return 7;
    remedy_wire_frame_header_t release{};
    if (!read_exact(endpoint, encoded, REMEDY_WIRE_HEADER_SIZE)) return 8;
    if (remedy_wire_frame_decode(encoded, &release) != REMEDY_OK) return 9;
    if (release.kind != REMEDY_WIRE_KIND_QUIESCE || release.payload_len != 0) return 10;
    if (!CloseHandle(endpoint)) return 11;
    return 0;
}
