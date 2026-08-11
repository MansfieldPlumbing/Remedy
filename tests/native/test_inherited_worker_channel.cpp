#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <unordered_set>
#include <vector>

#include "remedy/ports/channel_port.h"
#include "remedy/ports/worker_port.h"

extern "C" uintptr_t remedy_test_get_last_bootstrap_locator(void);

struct generation {
    remedy_channel_token_t channel{REMEDY_INVALID_CHANNEL_TOKEN};
    remedy_worker_token_t worker{REMEDY_INVALID_WORKER_TOKEN};
    uintptr_t locator{0};
};

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

static generation start_generation(const char* fixture, uint64_t sequence) {
    generation result{};
    std::string name = "receipt_a_" + std::to_string(GetCurrentProcessId()) + "_" +
                       std::to_string(sequence) + "_" + std::to_string(GetTickCount64());
    remedy_channel_config_t channel_config{name.c_str(), true};
    assert(channel_port_create(&channel_config, &result.channel) == REMEDY_OK);

    remedy_worker_config_t worker_config{};
    worker_config.executable_path = fixture;
    worker_config.bootstrap_channel = result.channel;
    assert(worker_port_start(&worker_config, &result.worker) == REMEDY_OK);
    assert(result.worker != REMEDY_INVALID_WORKER_TOKEN);
    result.locator = remedy_test_get_last_bootstrap_locator();
    assert(result.locator != 0);

    remedy_worker_token_t duplicate = 99;
    assert(worker_port_start(&worker_config, &duplicate) == REMEDY_ERR_REVOKING);
    assert(duplicate == REMEDY_INVALID_WORKER_TOKEN);
    return result;
}

static void connect_generation(const generation& value) {
    assert(channel_port_connect(value.channel, 2000) == REMEDY_OK);
}

static void exchange_ping(const generation& value, uint64_t request_id, uint64_t domain_handle) {
    remedy_wire_frame_header_t ping{};
    ping.magic = REMEDY_WIRE_MAGIC;
    ping.version = REMEDY_WIRE_VERSION;
    ping.kind = REMEDY_WIRE_KIND_PING;
    ping.header_len = REMEDY_WIRE_HEADER_SIZE;
    ping.request_id = request_id;
    ping.domain_handle = domain_handle;
    assert(channel_port_send_frame(value.channel, &ping, nullptr) == REMEDY_OK);

    remedy_wire_frame_header_t pong{};
    remedy_err_t read_result = channel_port_read_frame(value.channel, &pong, nullptr, 0);
    if (read_result != REMEDY_OK) {
        fprintf(stderr, "PING reply failed: request=%llu channel=%llu worker=%llu error=%d\n",
                static_cast<unsigned long long>(request_id),
                static_cast<unsigned long long>(value.channel),
                static_cast<unsigned long long>(value.worker),
                static_cast<int>(read_result));
        ExitProcess(30);
    }
    assert(pong.kind == REMEDY_WIRE_KIND_PONG);
    assert(pong.request_id == request_id);
    assert(pong.domain_handle == domain_handle);
}

static void release_generation(const generation& value, uint64_t request_id) {
    remedy_wire_frame_header_t release{};
    release.magic = REMEDY_WIRE_MAGIC;
    release.version = REMEDY_WIRE_VERSION;
    release.kind = REMEDY_WIRE_KIND_QUIESCE;
    release.header_len = REMEDY_WIRE_HEADER_SIZE;
    release.request_id = request_id;
    assert(channel_port_send_frame(value.channel, &release, nullptr) == REMEDY_OK);
}

static void retire_generation(const generation& value, bool expect_natural_death) {
    bool dead = false;
    if (expect_natural_death) {
        assert(worker_port_wait_for_death(value.worker, 2000, &dead) == REMEDY_OK);
        assert(dead);
    }
    assert(worker_port_terminate(value.worker) == REMEDY_OK);
    assert(worker_port_wait_for_death(value.worker, 2000, &dead) == REMEDY_OK);
    assert(dead);
    assert(worker_port_destroy(value.worker) == REMEDY_OK);
    assert(worker_port_destroy(value.worker) == REMEDY_ERR_INVALID_ARGUMENT);

    assert(channel_port_close(value.channel) == REMEDY_OK);
    assert(channel_port_destroy(value.channel) == REMEDY_OK);
    assert(channel_port_close(value.channel) == REMEDY_ERR_INVALID_ARGUMENT);
}

static std::vector<HANDLE> create_inheritance_canaries() {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    std::vector<HANDLE> result;
    for (size_t i = 0; i < 8; ++i) {
        HANDLE canary = CreateEventW(&security, TRUE, FALSE, nullptr);
        assert(canary != nullptr);
        result.push_back(canary);
    }
    return result;
}

static std::string format_canaries(const std::vector<HANDLE>& canaries) {
    std::string result;
    for (HANDLE canary : canaries) {
        if (!result.empty()) result.push_back(',');
        result.append(std::to_string(reinterpret_cast<uintptr_t>(canary)));
    }
    return result;
}

static void close_canaries(const std::vector<HANDLE>& canaries) {
    for (HANDLE canary : canaries) assert(CloseHandle(canary));
}

int main(int argc, char** argv) {
    assert(argc == 2);
    prove_locator_is_not_authority(argv[1], UINTPTR_MAX);
    const DWORD baseline_handles = process_handle_count();
    uint64_t sequence = 1;

    // Inheritance canary: ambient inheritable handles must not cross the allow-list.
    std::vector<HANDLE> canaries = create_inheritance_canaries();
    std::string canary_metadata = format_canaries(canaries);
    assert(SetEnvironmentVariableA("REMEDY_TEST_CANARY_HANDLES", canary_metadata.c_str()));
    generation canary_generation = start_generation(argv[1], sequence++);
    assert(SetEnvironmentVariableA("REMEDY_TEST_CANARY_HANDLES", nullptr));
    close_canaries(canaries);
    connect_generation(canary_generation);
    exchange_ping(canary_generation, 1, 1);
    release_generation(canary_generation, 1);
    retire_generation(canary_generation, true);

    // Crossed wires: correlation/domain integers may cross; endpoint possession may not.
    generation left = start_generation(argv[1], sequence++);
    generation right = start_generation(argv[1], sequence++);
    prove_locator_is_not_authority(argv[1], right.locator);
    prove_locator_is_not_authority(argv[1], left.locator);
    connect_generation(left);
    connect_generation(right);
    exchange_ping(left, 0xBBBBBBBBBBBBBBBBULL, 0x2222222222222222ULL);
    exchange_ping(right, 0xAAAAAAAAAAAAAAAAULL, 0x1111111111111111ULL);
    release_generation(left, 2);
    release_generation(right, 3);
    retire_generation(left, true);
    retire_generation(right, true);

    // Force raw HANDLE-number reuse and prove retired executive tokens stay dead.
    std::unordered_set<uintptr_t> retired_locators;
    std::vector<remedy_worker_token_t> retired_workers;
    std::vector<remedy_channel_token_t> retired_channels;
    bool handle_number_reused = false;
    for (size_t attempt = 0; attempt < 256 && !handle_number_reused; ++attempt) {
        generation value = start_generation(argv[1], sequence++);
        handle_number_reused = retired_locators.find(value.locator) != retired_locators.end();
        connect_generation(value);
        exchange_ping(value, sequence, sequence ^ 0x55AA55AA55AA55AAULL);
        release_generation(value, sequence);
        retired_locators.insert(value.locator);
        retired_workers.push_back(value.worker);
        retired_channels.push_back(value.channel);
        retire_generation(value, true);
    }
    assert(handle_number_reused);
    for (remedy_worker_token_t stale : retired_workers) {
        assert(worker_port_destroy(stale) == REMEDY_ERR_INVALID_ARGUMENT);
    }
    for (remedy_channel_token_t stale : retired_channels) {
        assert(channel_port_connect(stale, 1) == REMEDY_ERR_INVALID_ARGUMENT);
    }

    // Stress evidence: die before connect, after connect, after request, or after QUIESCE.
    static constexpr size_t stress_iterations = 2048;
    for (size_t i = 0; i < stress_iterations; ++i) {
        generation value = start_generation(argv[1], sequence++);
        switch (i & 3U) {
            case 0:
                retire_generation(value, false);
                break;
            case 1:
                connect_generation(value);
                retire_generation(value, false);
                break;
            case 2: {
                connect_generation(value);
                remedy_wire_frame_header_t ping{};
                ping.magic = REMEDY_WIRE_MAGIC;
                ping.version = REMEDY_WIRE_VERSION;
                ping.kind = REMEDY_WIRE_KIND_PING;
                ping.header_len = REMEDY_WIRE_HEADER_SIZE;
                ping.request_id = sequence;
                assert(channel_port_send_frame(value.channel, &ping, nullptr) == REMEDY_OK);
                retire_generation(value, false);
                break;
            }
            default:
                connect_generation(value);
                exchange_ping(value, sequence, sequence);
                release_generation(value, sequence);
                retire_generation(value, true);
                break;
        }
    }

    assert(process_handle_count() == baseline_handles);
    printf("Receipt A torture passed: handle reuse, crossed wires, canary isolation, and %zu lifecycle iterations.\n",
           stress_iterations);
    return 0;
}
