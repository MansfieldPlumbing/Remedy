#ifndef REVOKE_TYPES_H
#define REVOKE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t revoke_err_t;

#define REVOKE_OK                              0
#define REVOKE_ERR_INVALID_ARGUMENT           -1
#define REVOKE_ERR_OUT_OF_MEMORY              -2
#define REVOKE_ERR_HANDLE_STALE               -3
#define REVOKE_ERR_WRONG_TYPE                 -4
#define REVOKE_ERR_REVOKING                   -5
#define REVOKE_ERR_TIMEOUT                    -6
#define REVOKE_ERR_NOT_SUPPORTED              -7
#define REVOKE_ERR_IPC_FAILURE                -8
#define REVOKE_ERR_WAIT_FAILED                -9
#define REVOKE_ERR_WORKER_TERMINATION_FAILED  -10
#define REVOKE_ERR_CONTAINMENT_FAILED         -11
#define REVOKE_ERR_LATE_COMPLETION            -12

typedef enum {
    REVOKE_OBJECT_NONE    = 0,
    REVOKE_OBJECT_DOMAIN  = 1,
    REVOKE_OBJECT_ARENA   = 2,
    REVOKE_OBJECT_WORKER  = 3,
    REVOKE_OBJECT_CHANNEL = 4
} revoke_object_type_t;

typedef enum {
    REVOKE_SLOT_FREE     = 0,
    REVOKE_SLOT_LIVE     = 1,
    REVOKE_SLOT_REVOKING = 2,
    REVOKE_SLOT_RETIRED  = 3
} revoke_slot_state_t;

typedef enum {
    REVOKE_DOMAIN_ACTIVE      = 1,
    REVOKE_DOMAIN_REVOKING    = 2,
    REVOKE_DOMAIN_QUIESCING   = 3,
    REVOKE_DOMAIN_TERMINATING = 4,
    REVOKE_DOMAIN_DEAD        = 5,
    REVOKE_DOMAIN_FAILED      = 6
} revoke_domain_state_t;

#ifdef __cplusplus
}
#endif

#endif // REVOKE_TYPES_H
