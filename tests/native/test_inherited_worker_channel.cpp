#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

#include "remedy/ports/channel_port.h"
#include "remedy/ports/worker_port.h"

extern "C" uintptr_t remedy_test_get_last_bootstrap_locator(void);

static std::wstring widen(const char* text) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    assert(length > 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), length) == length);
    result.pop_back();
    return result;
}

static DWORD process_handle_count() {
    DWORD count = 0;
    assert(GetProcessHandleCount(GetCurrentProcess(), &count));
    return count;
}

static void prove_locator_is_not_authority(const char* fixture, uintptr_t locator) {
    std::wstring executable = widen(fixture);
    std::wstring command = L"\"" + executable + L"\" --remedy-channel-handle=" + std::to_wstring(locator);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    assert(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                          nullptr, nullptr, &startup, &process));
    assert(WaitForSingleObject(process.hProcess, 2000) == WAIT_OBJECT_0);
    DWORD exit_code = 0;
    assert(GetExitCodeProcess(process.hProcess, &exit_code));
    assert(exit_code == 3);
    assert(CloseHandle(process.hThread));
    assert(CloseHandle(process.hProcess));
}

int main(int argc, char** argv) {
    assert(argc == 2);
    prove_locator_is_not_authority(argv[1], UINTPTR_MAX);
    const DWORD baseline_handles = process_handle_count();

    const std::string name = "receipt_a_" + std::to_string(GetCurrentProcessId()) + "_" +
                             std::to_string(GetTickCount64());
    remedy_channel_config_t channel_config{name.c_str(), true};
    remedy_channel_token_t channel = REMEDY_INVALID_CHANNEL_TOKEN;
    assert(channel_port_create(&channel_config, &channel) == REMEDY_OK);

    remedy_worker_config_t worker_config{};
    worker_config.executable_path = argv[1];
    worker_config.bootstrap_channel = channel;
    remedy_worker_token_t worker = REMEDY_INVALID_WORKER_TOKEN;
    assert(worker_port_start(&worker_config, &worker) == REMEDY_OK);
    assert(worker != REMEDY_INVALID_WORKER_TOKEN);

    const uintptr_t locator = remedy_test_get_last_bootstrap_locator();
    assert(locator != 0);
    prove_locator_is_not_authority(argv[1], locator);

    std::wstring pipe_path = L"\\\\.\\pipe\\remedy-worker-" + widen(name.c_str());
    HANDLE reacquired = CreateFileW(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    assert(reacquired == INVALID_HANDLE_VALUE);
    assert(GetLastError() == ERROR_PIPE_BUSY);

    remedy_worker_token_t second_worker = 99;
    assert(worker_port_start(&worker_config, &second_worker) == REMEDY_ERR_REVOKING);
    assert(second_worker == REMEDY_INVALID_WORKER_TOKEN);

    assert(channel_port_connect(channel, 2000) == REMEDY_OK);
    remedy_wire_frame_header_t ping{};
    ping.magic = REMEDY_WIRE_MAGIC;
    ping.version = REMEDY_WIRE_VERSION;
    ping.kind = REMEDY_WIRE_KIND_PING;
    ping.header_len = REMEDY_WIRE_HEADER_SIZE;
    ping.request_id = 0x1122334455667788ULL;
    ping.domain_handle = 0x8877665544332211ULL;
    ping.checksum = remedy_adler32(nullptr, 0);
    assert(channel_port_send_frame(channel, &ping, nullptr) == REMEDY_OK);

    remedy_wire_frame_header_t pong{};
    remedy_err_t read_result = channel_port_read_frame(channel, &pong, nullptr, 0);
    if (read_result != REMEDY_OK) {
        fprintf(stderr, "reply read failed: remedy_err=%d\n", static_cast<int>(read_result));
        return 21;
    }
    assert(pong.kind == REMEDY_WIRE_KIND_PONG);
    assert(pong.request_id == ping.request_id);
    assert(pong.domain_handle == ping.domain_handle);

    remedy_wire_frame_header_t release = ping;
    release.kind = REMEDY_WIRE_KIND_QUIESCE;
    assert(channel_port_send_frame(channel, &release, nullptr) == REMEDY_OK);

    bool dead = false;
    assert(worker_port_wait_for_death(worker, 2000, &dead) == REMEDY_OK);
    assert(dead);
    assert(worker_port_terminate(worker) == REMEDY_OK);
    assert(worker_port_destroy(worker) == REMEDY_OK);
    assert(worker_port_destroy(worker) == REMEDY_ERR_INVALID_ARGUMENT);

    const remedy_channel_token_t stale_channel = channel;
    assert(channel_port_close(channel) == REMEDY_OK);
    assert(channel_port_destroy(channel) == REMEDY_OK);
    assert(channel_port_close(stale_channel) == REMEDY_ERR_INVALID_ARGUMENT);
    const DWORD final_handles = process_handle_count();
    if (final_handles != baseline_handles) {
        fprintf(stderr, "handle count mismatch: baseline=%lu final=%lu\n", baseline_handles, final_handles);
        return 22;
    }

    printf("Receipt A passed: inherited possession works; copied locator and name reacquisition do not.\n");
    return 0;
}
