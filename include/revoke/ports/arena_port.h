#ifndef REVOKE_ARENA_PORT_H
#define REVOKE_ARENA_PORT_H

#include "revoke/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t revoke_arena_token_t;
#define REVOKE_INVALID_ARENA_TOKEN 0ULL

/* All arena port operations return REVOKE_ERR_NOT_SUPPORTED in Milestone 1 */
static inline revoke_err_t arena_port_create(uint64_t size_bytes, revoke_arena_token_t* out_token) {
    if (out_token) *out_token = REVOKE_INVALID_ARENA_TOKEN;
    return REVOKE_ERR_NOT_SUPPORTED;
}

static inline revoke_err_t arena_port_map(revoke_arena_token_t token, uint32_t access_flags, void** out_ptr) {
    if (out_ptr) *out_ptr = NULL;
    return REVOKE_ERR_NOT_SUPPORTED;
}

static inline revoke_err_t arena_port_unmap(revoke_arena_token_t token, void* ptr) {
    return REVOKE_ERR_NOT_SUPPORTED;
}

static inline revoke_err_t arena_port_share(revoke_arena_token_t token, uint64_t worker_token, uint64_t* out_transfer_token) {
    if (out_transfer_token) *out_transfer_token = 0;
    return REVOKE_ERR_NOT_SUPPORTED;
}

static inline revoke_err_t arena_port_close(revoke_arena_token_t token) {
    return REVOKE_ERR_NOT_SUPPORTED;
}

#ifdef __cplusplus
}
#endif

#endif // REVOKE_ARENA_PORT_H
