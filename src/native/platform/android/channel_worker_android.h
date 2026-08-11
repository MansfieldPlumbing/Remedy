#ifndef REMEDY_CHANNEL_WORKER_ANDROID_H
#define REMEDY_CHANNEL_WORKER_ANDROID_H

#include "remedy/ports/channel_port.h"

remedy_err_t channel_android_issue_worker_endpoint(
    remedy_channel_token_t channel,
    int* out_worker_endpoint);

#endif
