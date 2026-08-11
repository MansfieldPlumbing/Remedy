#ifndef REMEDY_WORKER_LIFECYCLE_H
#define REMEDY_WORKER_LIFECYCLE_H

#include "remedy/ports/channel_port.h"
#include "remedy/ports/worker_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REMEDY_WORKER_LIFECYCLE_PAYLOAD_LIMIT 256U

typedef enum {
    REMEDY_WORKER_LIFECYCLE_EMPTY = 0,
    REMEDY_WORKER_LIFECYCLE_READY = 1,
    REMEDY_WORKER_LIFECYCLE_REQUEST_OUTSTANDING = 2,
    REMEDY_WORKER_LIFECYCLE_COMPLETED = 3,
    REMEDY_WORKER_LIFECYCLE_QUIESCE_ACKNOWLEDGED = 4,
    REMEDY_WORKER_LIFECYCLE_RETIRED = 5
} remedy_worker_lifecycle_state_t;

typedef struct {
    const char* executable_path;
    const char* channel_name;
} remedy_worker_lifecycle_config_t;

typedef struct {
    remedy_worker_token_t worker;
    remedy_channel_token_t channel;
    uint64_t generation;
    uint64_t correlation;
    remedy_worker_lifecycle_state_t state;
    bool ready_received;
    bool completion_received;
    bool quiesce_acknowledged;
    bool worker_exited;
    bool terminal_channel_observed;
} remedy_worker_lifecycle_t;

remedy_err_t remedy_worker_lifecycle_start(
    const remedy_worker_lifecycle_config_t* config,
    remedy_worker_lifecycle_t* out_lifecycle);

remedy_err_t remedy_worker_lifecycle_request(
    remedy_worker_lifecycle_t* lifecycle,
    uint64_t correlation,
    const void* request_payload,
    size_t request_payload_length,
    void* completion_payload,
    size_t completion_payload_capacity,
    size_t* out_completion_payload_length);

remedy_err_t remedy_worker_lifecycle_validate_completion(
    uint64_t expected_generation,
    uint64_t expected_correlation,
    const remedy_wire_frame_header_t* completion);

remedy_err_t remedy_worker_lifecycle_quiesce_and_retire(
    remedy_worker_lifecycle_t* lifecycle);

#ifdef __cplusplus
}
#endif

#endif // REMEDY_WORKER_LIFECYCLE_H
