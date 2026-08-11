#ifndef REMEDY_CHANNEL_WORKER_WIN32_H
#define REMEDY_CHANNEL_WORKER_WIN32_H

#include "remedy/ports/channel_port.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Platform-private authority transfer. The returned handle is owned by the
// caller and is already marked inheritable. Public consumers retain only the
// executive channel token; raw HANDLE identity never crosses the public ABI.
remedy_err_t channel_win32_issue_worker_endpoint(
    remedy_channel_token_t channel,
    HANDLE* out_worker_endpoint);

#endif
