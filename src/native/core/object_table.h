#ifndef REMEDY_CORE_OBJECT_TABLE_H
#define REMEDY_CORE_OBJECT_TABLE_H

#include "remedy/types.h"
#include "remedy/handle.h"

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cassert>
#include <chrono>

namespace remedy {

class object_table;

struct object_slot {
    std::atomic<uint32_t> generation{1};
    std::atomic<remedy_object_type_t> type{REMEDY_OBJECT_NONE};
    std::atomic<remedy_handle_t> owner_domain{REMEDY_INVALID_HANDLE};
    std::atomic<remedy_slot_state_t> state{REMEDY_SLOT_FREE};
    std::atomic<uint32_t> pin_count{0};
    void* resource_ptr{nullptr};

    std::mutex pin_mutex;
    std::condition_variable pin_cv;
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
            if (slot_->pin_count.fetch_sub(1, std::memory_order_release) == 1) {
                // Last pin released -> notify waiting remove()
                std::lock_guard<std::mutex> lock(slot_->pin_mutex);
                slot_->pin_cv.notify_all();
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

    remedy_handle_t insert(remedy_object_type_t type, remedy_handle_t owner_domain, void* resource_ptr);
    bool remove(remedy_handle_t handle);
    bool is_valid(remedy_handle_t handle, remedy_object_type_t expected_type = REMEDY_OBJECT_NONE) const;

    template <typename T>
    object_lease<T> acquire(remedy_handle_t handle, remedy_object_type_t expected_type = REMEDY_OBJECT_NONE) {
        uint32_t slot_idx = remedy_handle_slot(handle);
        uint32_t gen = remedy_handle_generation(handle);

        object_slot* slot = get_slot(slot_idx);
        if (!slot) return {};

        // Check state == LIVE before pinning
        if (slot->state.load(std::memory_order_acquire) != REMEDY_SLOT_LIVE) return {};

        // Pin slot
        slot->pin_count.fetch_add(1, std::memory_order_relaxed);

        // Re-validate state, generation, and type post-pin
        if (slot->state.load(std::memory_order_acquire) != REMEDY_SLOT_LIVE ||
            slot->generation.load(std::memory_order_relaxed) != gen ||
            (expected_type != REMEDY_OBJECT_NONE && slot->type.load(std::memory_order_relaxed) != expected_type)) {
            if (slot->pin_count.fetch_sub(1, std::memory_order_release) == 1) {
                std::lock_guard<std::mutex> lock(slot->pin_mutex);
                slot->pin_cv.notify_all();
            }
            return {};
        }

        return object_lease<T>(slot, static_cast<T*>(slot->resource_ptr));
    }

    uint32_t live_count() const;

private:
    object_slot* get_slot(uint32_t slot_idx) const;
    void ensure_capacity(uint32_t slot_idx);

    mutable std::mutex mutex_;
    object_slot* chunks_[MAX_CHUNKS]{nullptr};
    std::vector<uint32_t> free_list_;
    uint32_t high_watermark_{1};
};

} // namespace remedy

#endif // REMEDY_CORE_OBJECT_TABLE_H
