#include "remedy/worker_lifecycle.h"

#include <string.h>

static constexpr uint32_t REMEDY_WORKER_DEATH_TIMEOUT_MS = 5000;

static remedy_err_t retire_owned_resources(remedy_worker_lifecycle_t* lifecycle) {
    if (!lifecycle) return REMEDY_ERR_INVALID_ARGUMENT;
    bool cleanup_ok = true;

    if (lifecycle->worker != REMEDY_INVALID_WORKER_TOKEN) {
        remedy_err_t result = worker_port_terminate(lifecycle->worker);
        cleanup_ok = (result == REMEDY_OK) && cleanup_ok;

        bool died = false;
        result = worker_port_wait_for_death(
            lifecycle->worker,
            REMEDY_WORKER_DEATH_TIMEOUT_MS,
            &died);
        cleanup_ok = (result == REMEDY_OK && died) && cleanup_ok;

        result = worker_port_destroy(lifecycle->worker);
        cleanup_ok = (result == REMEDY_OK) && cleanup_ok;
        lifecycle->worker = REMEDY_INVALID_WORKER_TOKEN;
    }

    if (lifecycle->channel != REMEDY_INVALID_CHANNEL_TOKEN) {
        remedy_err_t result = channel_port_close(lifecycle->channel);
        cleanup_ok = (result == REMEDY_OK) && cleanup_ok;
        result = channel_port_destroy(lifecycle->channel);
        cleanup_ok = (result == REMEDY_OK) && cleanup_ok;
        lifecycle->channel = REMEDY_INVALID_CHANNEL_TOKEN;
    }

    lifecycle->state = REMEDY_WORKER_LIFECYCLE_RETIRED;
    return cleanup_ok ? REMEDY_OK : REMEDY_ERR_CONTAINMENT_FAILED;
}

static remedy_err_t fail_live_operation(
    remedy_worker_lifecycle_t* lifecycle,
    remedy_err_t operation_error) {
    remedy_err_t cleanup_result = retire_owned_resources(lifecycle);
    return cleanup_result == REMEDY_OK ? operation_error : REMEDY_ERR_CONTAINMENT_FAILED;
}

extern "C" remedy_err_t remedy_worker_lifecycle_start(
    const remedy_worker_lifecycle_config_t* config,
    remedy_worker_lifecycle_t* out_lifecycle) {
    if (!out_lifecycle) return REMEDY_ERR_INVALID_ARGUMENT;
    memset(out_lifecycle, 0, sizeof(*out_lifecycle));
    out_lifecycle->state = REMEDY_WORKER_LIFECYCLE_EMPTY;

    if (!config ||
        !config->executable_path || config->executable_path[0] == '\0' ||
        !config->channel_name || config->channel_name[0] == '\0') {
        return REMEDY_ERR_INVALID_ARGUMENT;
    }

    remedy_channel_config_t channel_config{};
    channel_config.channel_name = config->channel_name;
    channel_config.is_server = true;
    remedy_err_t result = channel_port_create(&channel_config, &out_lifecycle->channel);
    if (result != REMEDY_OK) return result;

    remedy_worker_config_t worker_config{};
    worker_config.executable_path = config->executable_path;
    worker_config.bootstrap_channel = out_lifecycle->channel;
    result = worker_port_start(&worker_config, &out_lifecycle->worker);
    if (result != REMEDY_OK) {
        remedy_err_t cleanup_result = retire_owned_resources(out_lifecycle);
        return cleanup_result == REMEDY_OK ? result : REMEDY_ERR_CONTAINMENT_FAILED;
    }
    out_lifecycle->generation = out_lifecycle->worker;

    result = channel_port_connect(out_lifecycle->channel, 2000);
    if (result != REMEDY_OK) return fail_live_operation(out_lifecycle, result);

    remedy_wire_frame_header_t ready{};
    result = channel_port_read_frame(out_lifecycle->channel, &ready, nullptr, 0);
    if (result != REMEDY_OK) return fail_live_operation(out_lifecycle, result);
    if (ready.kind != REMEDY_WIRE_KIND_READY ||
        ready.payload_len != 0 ||
        ready.checksum != 0 ||
        ready.request_id != 0 ||
        ready.domain_handle != 0) {
        return fail_live_operation(out_lifecycle, REMEDY_ERR_IPC_FAILURE);
    }

    out_lifecycle->ready_received = true;
    out_lifecycle->state = REMEDY_WORKER_LIFECYCLE_READY;
    return REMEDY_OK;
}

extern "C" remedy_err_t remedy_worker_lifecycle_validate_completion(
    uint64_t expected_generation,
    uint64_t expected_correlation,
    const remedy_wire_frame_header_t* completion) {
    if (!completion || expected_generation == 0 || expected_correlation == 0) {
        return REMEDY_ERR_INVALID_ARGUMENT;
    }
    if (completion->kind != REMEDY_WIRE_KIND_COMPLETION) return REMEDY_ERR_IPC_FAILURE;
    if (completion->domain_handle != expected_generation) return REMEDY_ERR_LATE_COMPLETION;
    if (completion->request_id != expected_correlation) return REMEDY_ERR_IPC_FAILURE;
    return REMEDY_OK;
}

extern "C" remedy_err_t remedy_worker_lifecycle_request(
    remedy_worker_lifecycle_t* lifecycle,
    uint64_t correlation,
    const void* request_payload,
    size_t request_payload_length,
    void* completion_payload,
    size_t completion_payload_capacity,
    size_t* out_completion_payload_length) {
    if (!out_completion_payload_length) return REMEDY_ERR_INVALID_ARGUMENT;
    *out_completion_payload_length = 0;
    if (!lifecycle ||
        lifecycle->state != REMEDY_WORKER_LIFECYCLE_READY ||
        lifecycle->worker == REMEDY_INVALID_WORKER_TOKEN ||
        lifecycle->channel == REMEDY_INVALID_CHANNEL_TOKEN ||
        correlation == 0 ||
        request_payload_length > REMEDY_WORKER_LIFECYCLE_PAYLOAD_LIMIT ||
        completion_payload_capacity > REMEDY_WORKER_LIFECYCLE_PAYLOAD_LIMIT ||
        (request_payload_length != 0 && !request_payload) ||
        (completion_payload_capacity != 0 && !completion_payload)) {
        return lifecycle && lifecycle->state == REMEDY_WORKER_LIFECYCLE_COMPLETED
            ? REMEDY_ERR_NOT_SUPPORTED
            : REMEDY_ERR_INVALID_ARGUMENT;
    }

    lifecycle->correlation = correlation;
    lifecycle->state = REMEDY_WORKER_LIFECYCLE_REQUEST_OUTSTANDING;

    remedy_wire_frame_header_t request{};
    request.kind = REMEDY_WIRE_KIND_REQUEST;
    request.payload_len = static_cast<uint32_t>(request_payload_length);
    request.request_id = correlation;
    request.domain_handle = lifecycle->generation;
    remedy_err_t result = channel_port_send_frame(lifecycle->channel, &request, request_payload);
    if (result != REMEDY_OK) return fail_live_operation(lifecycle, result);

    remedy_wire_frame_header_t completion{};
    result = channel_port_read_frame(
        lifecycle->channel,
        &completion,
        completion_payload,
        completion_payload_capacity);
    if (result != REMEDY_OK) return fail_live_operation(lifecycle, result);

    result = remedy_worker_lifecycle_validate_completion(
        lifecycle->generation,
        correlation,
        &completion);
    if (result != REMEDY_OK) return fail_live_operation(lifecycle, result);

    lifecycle->completion_received = true;
    lifecycle->state = REMEDY_WORKER_LIFECYCLE_COMPLETED;
    *out_completion_payload_length = completion.payload_len;
    return REMEDY_OK;
}

extern "C" remedy_err_t remedy_worker_lifecycle_quiesce_and_retire(
    remedy_worker_lifecycle_t* lifecycle) {
    if (!lifecycle ||
        lifecycle->state != REMEDY_WORKER_LIFECYCLE_COMPLETED ||
        lifecycle->worker == REMEDY_INVALID_WORKER_TOKEN ||
        lifecycle->channel == REMEDY_INVALID_CHANNEL_TOKEN) {
        return REMEDY_ERR_INVALID_ARGUMENT;
    }

    remedy_wire_frame_header_t quiesce{};
    quiesce.kind = REMEDY_WIRE_KIND_QUIESCE;
    quiesce.request_id = lifecycle->correlation;
    quiesce.domain_handle = lifecycle->generation;
    remedy_err_t result = channel_port_send_frame(lifecycle->channel, &quiesce, nullptr);
    if (result != REMEDY_OK) return fail_live_operation(lifecycle, result);

    remedy_wire_frame_header_t acknowledgement{};
    result = channel_port_read_frame(lifecycle->channel, &acknowledgement, nullptr, 0);
    if (result != REMEDY_OK) return fail_live_operation(lifecycle, result);
    if (acknowledgement.kind != REMEDY_WIRE_KIND_QUIESCE_ACK ||
        acknowledgement.request_id != lifecycle->correlation ||
        acknowledgement.domain_handle != lifecycle->generation ||
        acknowledgement.payload_len != 0 ||
        acknowledgement.checksum != 0) {
        return fail_live_operation(lifecycle, REMEDY_ERR_IPC_FAILURE);
    }
    lifecycle->quiesce_acknowledged = true;
    lifecycle->state = REMEDY_WORKER_LIFECYCLE_QUIESCE_ACKNOWLEDGED;

    bool died = false;
    result = worker_port_wait_for_death(
        lifecycle->worker,
        REMEDY_WORKER_DEATH_TIMEOUT_MS,
        &died);
    if (result != REMEDY_OK || !died) {
        return fail_live_operation(
            lifecycle,
            result == REMEDY_OK ? REMEDY_ERR_WAIT_FAILED : result);
    }
    lifecycle->worker_exited = true;

    remedy_wire_frame_header_t terminal_probe{};
    result = channel_port_read_frame(lifecycle->channel, &terminal_probe, nullptr, 0);
    if (result != REMEDY_ERR_IPC_FAILURE) {
        return fail_live_operation(
            lifecycle,
            result == REMEDY_OK ? REMEDY_ERR_IPC_FAILURE : result);
    }
    lifecycle->terminal_channel_observed = true;

    result = retire_owned_resources(lifecycle);
    return result;
}
