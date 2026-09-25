#ifndef REVOKE_HANDLE_H
#define REVOKE_HANDLE_H

#include "revoke/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 64-bit handle: 32-bit generation, 32-bit slot index */
typedef uint64_t revoke_handle_t;

#define REVOKE_INVALID_HANDLE 0ULL

static inline uint32_t revoke_handle_slot(revoke_handle_t handle) {
    return (uint32_t)(handle & 0xFFFFFFFFULL);
}

static inline uint32_t revoke_handle_generation(revoke_handle_t handle) {
    return (uint32_t)(handle >> 32);
}

static inline revoke_handle_t revoke_handle_make(uint32_t generation, uint32_t slot) {
    return (((uint64_t)generation) << 32) | ((uint64_t)slot);
}

#ifdef __cplusplus
}
#endif

#endif // REVOKE_HANDLE_H
