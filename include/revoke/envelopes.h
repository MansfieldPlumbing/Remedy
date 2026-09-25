#ifndef REVOKE_ENVELOPES_H
#define REVOKE_ENVELOPES_H

#include "revoke/types.h"
#include "revoke/handle.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

typedef struct {
    uint64_t request_id;
    revoke_handle_t domain_handle;
    revoke_handle_t lease_handle;
    uint32_t operation_id;
    revoke_handle_t payload_arena_handle;
    uint32_t payload_offset;
    uint32_t payload_length;
    uint64_t deadline_ms;
    revoke_handle_t reply_channel_handle;
} revoke_request_envelope_t;

typedef struct {
    uint64_t request_id;
    int32_t  status_code;
    revoke_handle_t result_arena_handle;
    uint32_t result_offset;
    uint32_t result_length;
    uint64_t receipt_reference;
} revoke_completion_envelope_t;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif // REVOKE_ENVELOPES_H
