#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <unordered_set>
#include <vector>

#include "remedy/ports/channel_port.h"
#include "remedy/ports/worker_port.h"

extern char** environ;
extern "C" int remedy_test_get_last_bootstrap_descriptor(void);

struct generation {
    remedy_channel_token_t channel{REMEDY_INVALID_CHANNEL_TOKEN};
    remedy_worker_token_t worker{REMEDY_INVALID_WORKER_TOKEN};
    int source_descriptor{-1};
};

static size_t open_descriptor_count() {
    DIR* directory = opendir("/proc/self/fd");
    assert(directory != nullptr);
    size_t count = 0;
    for (;;) {
        errno = 0;
        dirent* item = readdir(directory);
        if (!item) {
            assert(errno == 0);
            break;
        }
        if (item->d_name[0] == '.' &&
            (item->d_name[1] == '\0' || (item->d_name[1] == '.' && item->d_name[2] == '\0'))) {
            continue;
        }
        ++count;
    }
    assert(closedir(directory) == 0);
    return count;
}

static generation start_generation(const char* fixture, uint64_t sequence) {
    generation result{};
    std::string name = "receipt_a_" + std::to_string(getpid()) + "_" + std::to_string(sequence);
    remedy_channel_config_t channel_config{name.c_str(), true};
    assert(channel_port_create(&channel_config, &result.channel) == REMEDY_OK);
    remedy_worker_config_t worker_config{};
    worker_config.executable_path = fixture;
    worker_config.bootstrap_channel = result.channel;
    assert(worker_port_start(&worker_config, &result.worker) == REMEDY_OK);
    result.source_descriptor = remedy_test_get_last_bootstrap_descriptor();
    assert(result.source_descriptor >= 0);
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
    ping.kind = REMEDY_WIRE_KIND_PING;
    ping.request_id = request_id;
    ping.domain_handle = domain_handle;
    assert(channel_port_send_frame(value.channel, &ping, nullptr) == REMEDY_OK);
    remedy_wire_frame_header_t pong{};
    assert(channel_port_read_frame(value.channel, &pong, nullptr, 0) == REMEDY_OK);
    assert(pong.kind == REMEDY_WIRE_KIND_PONG);
    assert(pong.request_id == request_id);
    assert(pong.domain_handle == domain_handle);
}

static void release_generation(const generation& value, uint64_t request_id) {
    remedy_wire_frame_header_t release{};
    release.kind = REMEDY_WIRE_KIND_QUIESCE;
    release.request_id = request_id;
    assert(channel_port_send_frame(value.channel, &release, nullptr) == REMEDY_OK);
}

static void retire_generation(const generation& value, bool natural_death) {
    bool dead = false;
    if (natural_death) {
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
    assert(channel_port_connect(value.channel, 1) == REMEDY_ERR_INVALID_ARGUMENT);
}

static void prove_descriptor_number_is_not_authority(const char* fixture) {
    posix_spawnattr_t attributes{};
    assert(posix_spawnattr_init(&attributes) == 0);
    assert(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT) == 0);
    char* const argv[] = {const_cast<char*>(fixture), nullptr};
    pid_t pid = -1;
    assert(posix_spawn(&pid, fixture, nullptr, &attributes, argv, environ) == 0);
    assert(posix_spawnattr_destroy(&attributes) == 0);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 3);
}

static std::vector<int> create_canaries() {
    std::vector<int> result;
    for (size_t i = 0; i < 8; ++i) {
        int fd = eventfd(0, 0);
        assert(fd >= 0);
        int flags = fcntl(fd, F_GETFD);
        assert(flags >= 0);
        assert(fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == 0);
        result.push_back(fd);
    }
    return result;
}

static void close_canaries(const std::vector<int>& canaries) {
    for (int fd : canaries) assert(close(fd) == 0);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const char* fixture = argv[1];
    prove_descriptor_number_is_not_authority(fixture);
    const size_t baseline_descriptors = open_descriptor_count();
    uint64_t sequence = 1;

    // Partial launch consumes and then reclaims the detached endpoint.
    {
        std::string name = "receipt_a_failed_launch";
        remedy_channel_config_t channel_config{name.c_str(), true};
        remedy_channel_token_t channel = REMEDY_INVALID_CHANNEL_TOKEN;
        assert(channel_port_create(&channel_config, &channel) == REMEDY_OK);
        remedy_worker_config_t failed_config{};
        failed_config.executable_path = "/data/local/tmp/remedy-does-not-exist";
        failed_config.bootstrap_channel = channel;
        remedy_worker_token_t worker = 99;
        assert(worker_port_start(&failed_config, &worker) == REMEDY_ERR_IPC_FAILURE);
        assert(worker == REMEDY_INVALID_WORKER_TOKEN);
        assert(channel_port_close(channel) == REMEDY_OK);
        assert(channel_port_destroy(channel) == REMEDY_OK);
        assert(open_descriptor_count() == baseline_descriptors);
    }

    remedy_worker_config_t arguments_config{};
    arguments_config.executable_path = fixture;
    arguments_config.arguments = "--ambient-authority";
    remedy_worker_token_t arguments_worker = 99;
    assert(worker_port_start(&arguments_config, &arguments_worker) == REMEDY_ERR_NOT_SUPPORTED);
    assert(arguments_worker == REMEDY_INVALID_WORKER_TOKEN);

    std::vector<int> canaries = create_canaries();
    generation canary = start_generation(fixture, sequence++);
    close_canaries(canaries);
    connect_generation(canary);
    exchange_ping(canary, 1, 1);
    release_generation(canary, 1);
    retire_generation(canary, true);

    generation left = start_generation(fixture, sequence++);
    generation right = start_generation(fixture, sequence++);
    connect_generation(left);
    connect_generation(right);
    exchange_ping(left, 0xBBBBBBBBBBBBBBBBULL, 0x2222222222222222ULL);
    exchange_ping(right, 0xAAAAAAAAAAAAAAAAULL, 0x1111111111111111ULL);
    release_generation(left, 2);
    release_generation(right, 3);
    retire_generation(left, true);
    retire_generation(right, true);

    std::unordered_set<int> retired_descriptors;
    std::vector<remedy_worker_token_t> stale_workers;
    std::vector<remedy_channel_token_t> stale_channels;
    bool descriptor_reused = false;
    for (size_t attempt = 0; attempt < 256 && !descriptor_reused; ++attempt) {
        generation value = start_generation(fixture, sequence++);
        descriptor_reused = retired_descriptors.find(value.source_descriptor) != retired_descriptors.end();
        connect_generation(value);
        exchange_ping(value, sequence, sequence);
        release_generation(value, sequence);
        retired_descriptors.insert(value.source_descriptor);
        stale_workers.push_back(value.worker);
        stale_channels.push_back(value.channel);
        retire_generation(value, true);
    }
    assert(descriptor_reused);
    for (auto token : stale_workers) assert(worker_port_destroy(token) == REMEDY_ERR_INVALID_ARGUMENT);
    for (auto token : stale_channels) assert(channel_port_connect(token, 1) == REMEDY_ERR_INVALID_ARGUMENT);

    static constexpr size_t stress_iterations = 2048;
    for (size_t i = 0; i < stress_iterations; ++i) {
        generation value = start_generation(fixture, sequence++);
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
                ping.kind = REMEDY_WIRE_KIND_PING;
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

    assert(open_descriptor_count() == baseline_descriptors);

    printf("Android Receipt A passed: fixed fd 3, canary isolation, descriptor reuse, crossed wires, and %zu lifecycles.\n",
           stress_iterations);
    return 0;
}
