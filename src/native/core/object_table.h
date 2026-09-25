#ifndef REVOKE_CORE_OBJECT_TABLE_H
#define REVOKE_CORE_OBJECT_TABLE_H

#include "revoke/types.h"
#include "revoke/handle.h"

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cassert>
#include <chrono>
#include <cstdlib>

namespace revoke {

using revoke_deleter_fn = void (*)(void* resource) noexcept;

class object_table;

struct object_slot {
    uint32_t generation{1};
    revoke_object_type_t type{REVOKE_OBJECT_NONE};
    revoke_handle_t owner_domain{REVOKE_INVALID_HANDLE};
    revoke_slot_state_t state{REVOKE_SLOT_FREE};
    uint32_t pin_count{0};
    void* resource_ptr{nullptr};
    revoke_deleter_fn deleter{nullptr};
    bool rundown_active{false};

    mutable std::mutex slot_mutex;
    std::condition_variable slot_cv;
};

template <typename T>
class object_lease {
public:
    object_lease() = default;
    object_lease(object_slot* slot, T* ptr) : slot_(slot), ptr_(ptr) {}

    ~object_lease() {
        reset();
    }

    object_lease(const object_lease&) = delete;
    object_lease& operator=(const object_lease&) = delete;

    object_lease(object_lease&& other) noexcept : slot_(other.slot_), ptr_(other.ptr_) {
        other.slot_ = nullptr;
        other.ptr_ = nullptr;
    }

    object_lease& operator=(object_lease&& other) noexcept {
        if (this != &other) {
            reset();
            slot_ = other.slot_;
            ptr_ = other.ptr_;
            other.slot_ = nullptr;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    explicit operator bool() const { return ptr_ != nullptr; }
    T* operator->() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* get() const { return ptr_; }

    void reset() {
        if (slot_) {
            std::lock_guard<std::mutex> lock(slot_->slot_mutex);
            if (slot_->pin_count == 0) {
                std::abort(); // Unconditional fail-fast if pin_count == 0 before decrement
            }
            slot_->pin_count--;
            if (slot_->pin_count == 0) {
                slot_->slot_cv.notify_all();
            }
            slot_ = nullptr;
            ptr_ = nullptr;
        }
    }

private:
    object_slot* slot_{nullptr};
    T* ptr_{nullptr};
};

class object_table {
public:
    static constexpr size_t CHUNK_SIZE = 64;
    static constexpr size_t MAX_CHUNKS = 1024;

    object_table();
    ~object_table();

    revoke_handle_t insert(revoke_object_type_t type, revoke_handle_t owner_domain, void* resource_ptr, revoke_deleter_fn deleter);
    revoke_err_t remove(revoke_handle_t handle, uint32_t timeout_ms = 2000);
    bool is_valid(revoke_handle_t handle, revoke_object_type_t expected_type = REVOKE_OBJECT_NONE) const;

    template <typename T>
    object_lease<T> acquire(revoke_handle_t handle, revoke_object_type_t expected_type = REVOKE_OBJECT_NONE, revoke_err_t* out_err = nullptr) {
        uint32_t slot_idx = revoke_handle_slot(handle);
        uint32_t gen = revoke_handle_generation(handle);

        object_slot* slot = get_slot(slot_idx);
        if (!slot) {
            if (out_err) *out_err = REVOKE_ERR_HANDLE_STALE;
            return {};
        }

        std::lock_guard<std::mutex> lock(slot->slot_mutex);

        // 1. Generation mismatch -> REVOKE_ERR_HANDLE_STALE
        if (slot->generation != gen) {
            if (out_err) *out_err = REVOKE_ERR_HANDLE_STALE;
            return {};
        }

        // 2. State FREE or RETIRED -> REVOKE_ERR_HANDLE_STALE
        if (slot->state == REVOKE_SLOT_FREE || slot->state == REVOKE_SLOT_RETIRED) {
            if (out_err) *out_err = REVOKE_ERR_HANDLE_STALE;
            return {};
        }

        // 3. State REVOKING -> REVOKE_ERR_REVOKING
        if (slot->state == REVOKE_SLOT_REVOKING) {
            if (out_err) *out_err = REVOKE_ERR_REVOKING;
            return {};
        }

        // 4. Expected-type mismatch -> REVOKE_ERR_WRONG_TYPE
        if (expected_type != REVOKE_OBJECT_NONE && slot->type != expected_type) {
            if (out_err) *out_err = REVOKE_ERR_WRONG_TYPE;
            return {};
        }

        // 5. State LIVE, generation valid, type valid, resource valid -> increment pin and return REVOKE_OK
        if (slot->state != REVOKE_SLOT_LIVE || slot->resource_ptr == nullptr) {
            std::abort(); // Unconditional fail-fast on malformed internal state
        }

        slot->pin_count++;
        if (out_err) *out_err = REVOKE_OK;
        return object_lease<T>(slot, static_cast<T*>(slot->resource_ptr));
    }

    uint32_t live_count() const;

private:
    object_slot* get_slot(uint32_t slot_idx) const;
    void ensure_capacity(uint32_t slot_idx);

    mutable std::mutex mutex_;
    std::atomic<object_slot*> chunks_[MAX_CHUNKS]{nullptr};
    std::vector<uint32_t> free_list_;
    uint32_t high_watermark_{1};
};

} // namespace revoke

#endif // REVOKE_CORE_OBJECT_TABLE_H
