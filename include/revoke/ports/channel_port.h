#ifndef REVOKE_CHANNEL_PORT_H
#define REVOKE_CHANNEL_PORT_H

#include "revoke/types.h"
#include "revoke/wire_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t revoke_channel_token_t;

#define REVOKE_INVALID_CHANNEL_TOKEN 0ULL

typedef struct {
    const char* channel_name;
    bool        is_server;
} revoke_channel_config_t;

revoke_err_t channel_port_create(const revoke_channel_config_t* config, revoke_channel_token_t* out_token);
revoke_err_t channel_port_connect(revoke_channel_token_t token, uint32_t timeout_ms);
revoke_err_t channel_port_send_frame(revoke_channel_token_t token, const revoke_wire_frame_header_t* header, const void* payload);
revoke_err_t channel_port_read_frame(revoke_channel_token_t token, revoke_wire_frame_header_t* out_header, void* payload_buffer, size_t max_payload_len);
revoke_err_t channel_port_close(revoke_channel_token_t token);
revoke_err_t channel_port_destroy(revoke_channel_token_t token);

#ifdef __cplusplus
}
#endif

#endif // REVOKE_CHANNEL_PORT_H
