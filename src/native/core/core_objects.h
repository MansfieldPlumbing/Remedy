#ifndef REVOKE_CORE_OBJECTS_H
#define REVOKE_CORE_OBJECTS_H

#include "revoke/types.h"
#include "revoke/handle.h"
#include "revoke/ports/worker_port.h"
#include "revoke/ports/channel_port.h"

namespace revoke {

class domain;

class worker_object {
public:
    explicit worker_object(revoke_worker_token_t token) : token_(token) {}
    ~worker_object() {
        if (token_ != REVOKE_INVALID_WORKER_TOKEN) {
            worker_port_destroy(token_);
            token_ = REVOKE_INVALID_WORKER_TOKEN;
        }
    }

    revoke_worker_token_t token() const { return token_; }

private:
    revoke_worker_token_t token_{REVOKE_INVALID_WORKER_TOKEN};
};

class channel_object {
public:
    explicit channel_object(revoke_channel_token_t token) : token_(token) {}
    ~channel_object() {
        if (token_ != REVOKE_INVALID_CHANNEL_TOKEN) {
            channel_port_close(token_);
            channel_port_destroy(token_);
            token_ = REVOKE_INVALID_CHANNEL_TOKEN;
        }
    }

    revoke_channel_token_t token() const { return token_; }

private:
    revoke_channel_token_t token_{REVOKE_INVALID_CHANNEL_TOKEN};
};

class domain_object {
public:
    explicit domain_object(domain* d) : domain_ptr_(d) {}
    ~domain_object(); // Implemented after domain declaration

    domain* get() const { return domain_ptr_; }

private:
    domain* domain_ptr_{nullptr};
};

} // namespace revoke

#endif // REVOKE_CORE_OBJECTS_H
