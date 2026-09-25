#ifndef REVOKE_SLOT_PORT_H
#define REVOKE_SLOT_PORT_H

#include "revoke/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t slot_id;
    uint32_t version_major;
    uint32_t version_minor;
    bool     is_active;
} revoke_slot_info_t;

/* All slot operations return REVOKE_ERR_NOT_SUPPORTED in Milestone 1 */
static inline revoke_err_t slot_port_stage(uint32_t slot_id, const char* package_path) {
    return REVOKE_ERR_NOT_SUPPORTED;
}

static inline revoke_err_t slot_port_verify(uint32_t slot_id) {
    return REVOKE_ERR_NOT_SUPPORTED;
}

static inline revoke_err_t slot_port_probe(uint32_t slot_id, revoke_slot_info_t* out_info) {
    if (out_info) {
        out_info->slot_id = slot_id;
        out_info->version_major = 0;
        out_info->version_minor = 0;
        out_info->is_active = false;
    }
    return REVOKE_ERR_NOT_SUPPORTED;
}

static inline revoke_err_t slot_port_activate(uint32_t slot_id) {
    return REVOKE_ERR_NOT_SUPPORTED;
}

static inline revoke_err_t slot_port_rollback(uint32_t slot_id) {
    return REVOKE_ERR_NOT_SUPPORTED;
}

static inline revoke_err_t slot_port_retire(uint32_t slot_id) {
    return REVOKE_ERR_NOT_SUPPORTED;
}

#ifdef __cplusplus
}
#endif

#endif // REVOKE_SLOT_PORT_H
