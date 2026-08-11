#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

#include "remedy/worker_lifecycle.h"

static DWORD process_handle_count() {
    DWORD count = 0;
    assert(GetProcessHandleCount(GetCurrentProcess(), &count));
    return count;
}

static std::wstring widen(const char* text) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    assert(length > 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), length) == length);
    result.pop_back();
    return result;
}

static void warm_process_creation_without_authority(const char* worker_path) {
    const std::wstring executable = widen(worker_path);
    std::wstring command = L"\"" + executable + L"\" --remedy-channel-handle=18446744073709551615";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    assert(CreateProcessW(
        executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
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

    warm_process_creation_without_authority(argv[1]);

    remedy_worker_config_t unsupported{};
    unsupported.executable_path = argv[1];
    unsupported.arguments = "--ambient-worker-option";
    remedy_worker_token_t unsupported_worker = 99;
    assert(worker_port_start(&unsupported, &unsupported_worker) == REMEDY_ERR_NOT_SUPPORTED);
    assert(unsupported_worker == REMEDY_INVALID_WORKER_TOKEN);

    const DWORD baseline_handles = process_handle_count();
    const std::string channel_name =
        "receipt_b_m1_" + std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());
    remedy_worker_lifecycle_config_t config{argv[1], channel_name.c_str()};
    remedy_worker_lifecycle_t lifecycle{};

    assert(remedy_worker_lifecycle_start(&config, &lifecycle) == REMEDY_OK);
    assert(lifecycle.worker != REMEDY_INVALID_WORKER_TOKEN);
    assert(lifecycle.channel != REMEDY_INVALID_CHANNEL_TOKEN);
    assert(lifecycle.generation == lifecycle.worker);
    assert(lifecycle.generation != 0);
    assert(lifecycle.ready_received);
    assert(lifecycle.state == REMEDY_WORKER_LIFECYCLE_READY);

    const remedy_worker_token_t stale_worker = lifecycle.worker;
    const remedy_channel_token_t stale_channel = lifecycle.channel;
    const uint64_t generation = lifecycle.generation;
    const uint64_t correlation = 0x5245434549505442ULL;

    remedy_wire_frame_header_t forged_completion{};
    forged_completion.kind = REMEDY_WIRE_KIND_COMPLETION;
    forged_completion.request_id = correlation;
    forged_completion.domain_handle = generation + 1;
    assert(remedy_worker_lifecycle_validate_completion(
        generation,
        correlation,
        &forged_completion) == REMEDY_ERR_LATE_COMPLETION);

    static const uint8_t request[] = {
        0x00, 0x01, 0x41, 0x5a, 0x7f, 0x80, 0xfe, 0xff
    };
    uint8_t completion[sizeof(request)]{};
    size_t completion_length = 99;
    assert(remedy_worker_lifecycle_request(
        &lifecycle,
        correlation,
        request,
        sizeof(request),
        completion,
        sizeof(completion),
        &completion_length) == REMEDY_OK);
    assert(completion_length == sizeof(request));
    for (size_t i = 0; i < sizeof(request); ++i) {
        assert(completion[i] == static_cast<uint8_t>(request[i] ^ 0x5aU));
    }
    assert(lifecycle.completion_received);
    assert(lifecycle.correlation == correlation);
    assert(lifecycle.state == REMEDY_WORKER_LIFECYCLE_COMPLETED);

    completion_length = 99;
    assert(remedy_worker_lifecycle_request(
        &lifecycle,
        correlation + 1,
        request,
        sizeof(request),
        completion,
        sizeof(completion),
        &completion_length) == REMEDY_ERR_NOT_SUPPORTED);
    assert(completion_length == 0);

    assert(remedy_worker_lifecycle_quiesce_and_retire(&lifecycle) == REMEDY_OK);
    assert(lifecycle.quiesce_acknowledged);
    assert(lifecycle.worker_exited);
    assert(lifecycle.terminal_channel_observed);
    assert(lifecycle.state == REMEDY_WORKER_LIFECYCLE_RETIRED);
    assert(lifecycle.worker == REMEDY_INVALID_WORKER_TOKEN);
    assert(lifecycle.channel == REMEDY_INVALID_CHANNEL_TOKEN);

    bool died = false;
    assert(worker_port_wait_for_death(stale_worker, 1, &died) == REMEDY_ERR_INVALID_ARGUMENT);
    assert(!died);
    assert(worker_port_destroy(stale_worker) == REMEDY_ERR_INVALID_ARGUMENT);
    assert(channel_port_connect(stale_channel, 1) == REMEDY_ERR_INVALID_ARGUMENT);
    assert(channel_port_close(stale_channel) == REMEDY_ERR_INVALID_ARGUMENT);
    const DWORD final_handles = process_handle_count();
    if (final_handles != baseline_handles) {
        fprintf(stderr, "Handle restoration failed: baseline=%lu final=%lu\n",
                static_cast<unsigned long>(baseline_handles),
                static_cast<unsigned long>(final_handles));
        return 20;
    }

    puts("Receipt B M1 passed: READY, correlated completion, explicit quiesce, terminal closure, retirement, and handle restoration.");
    return 0;
}
