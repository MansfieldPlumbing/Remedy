#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

#include "remedy/worker_lifecycle.h"

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
            (item->d_name[1] == '\0' ||
             (item->d_name[1] == '.' && item->d_name[2] == '\0'))) {
            continue;
        }
        ++count;
    }
    assert(closedir(directory) == 0);
    return count;
}

int main(int argc, char** argv) {
    assert(argc == 2);

    remedy_worker_config_t unsupported{};
    unsupported.executable_path = argv[1];
    unsupported.arguments = "--ambient-worker-option";
    remedy_worker_token_t unsupported_worker = 99;
    assert(worker_port_start(&unsupported, &unsupported_worker) == REMEDY_ERR_NOT_SUPPORTED);
    assert(unsupported_worker == REMEDY_INVALID_WORKER_TOKEN);

    const size_t baseline_descriptors = open_descriptor_count();
    const std::string channel_name = "receipt_b_m1_" + std::to_string(getpid());
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
    assert(open_descriptor_count() == baseline_descriptors);

    puts("Receipt B M1 passed: READY, correlated completion, explicit quiesce, terminal closure, retirement, and descriptor restoration.");
    return 0;
}
