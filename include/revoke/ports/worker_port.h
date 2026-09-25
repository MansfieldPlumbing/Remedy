#ifndef REVOKE_WORKER_PORT_H
#define REVOKE_WORKER_PORT_H

#include "revoke/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t revoke_worker_token_t;

#define REVOKE_INVALID_WORKER_TOKEN 0ULL

typedef struct {
    const char* executable_path;
    const char* arguments;
    const char* working_directory;
    const char* channel_nonce;
    uint32_t    timeout_ms;
} revoke_worker_config_t;

revoke_err_t worker_port_start(const revoke_worker_config_t* config, revoke_worker_token_t* out_token);
revoke_err_t worker_port_request_quiescence(revoke_worker_token_t token);
revoke_err_t worker_port_terminate(revoke_worker_token_t token);
revoke_err_t worker_port_wait_for_death(revoke_worker_token_t token, uint32_t timeout_ms, bool* out_died);
revoke_err_t worker_port_destroy(revoke_worker_token_t token);

#ifdef __cplusplus
}
#endif

#endif // REVOKE_WORKER_PORT_H
