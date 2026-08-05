#include "object_table.h"
#include "core_objects.h"

namespace remedy {

object_table::object_table() {
    ensure_capacity(CHUNK_SIZE - 1);
}

object_table::~object_table() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t c = 0; c < MAX_CHUNKS; ++c) {
        if (chunks_[c]) {
            for (size_t i = 0; i < CHUNK_SIZE; ++i) {
                object_slot& slot = chunks_[c][i];
                if (slot.state.load(std::memory_order_relaxed) == REMEDY_SLOT_LIVE && slot.resource_ptr) {
                    remedy_object_type_t t = slot.type.load(std::memory_order_relaxed);
                    if (t == REMEDY_OBJECT_WORKER) {
                        delete static_cast<worker_object*>(slot.resource_ptr);
                    } else if (t == REMEDY_OBJECT_CHANNEL) {
                        delete static_cast<channel_object*>(slot.resource_ptr);
                    } else if (t == REMEDY_OBJECT_DOMAIN) {
                        delete static_cast<domain_object*>(slot.resource_ptr);
                    }
                    slot.resource_ptr = nullptr;
                }
            }
            delete[] chunks_[c];
            chunks_[c] = nullptr;
        }
    }
}

object_slot* object_table::get_slot(uint32_t slot_idx) const {
    if (slot_idx == 0) return nullptr;
    uint32_t chunk_idx = slot_idx / CHUNK_SIZE;
    uint32_t offset = slot_idx % CHUNK_SIZE;
    if (chunk_idx >= MAX_CHUNKS) return nullptr;
    object_slot* chunk = chunks_[chunk_idx];
    if (!chunk) return nullptr;
    return &chunk[offset];
}

void object_table::ensure_capacity(uint32_t slot_idx) {
    uint32_t chunk_idx = slot_idx / CHUNK_SIZE;
    if (chunk_idx >= MAX_CHUNKS) return;
    if (!chunks_[chunk_idx]) {
        chunks_[chunk_idx] = new object_slot[CHUNK_SIZE]();
    }
}

remedy_handle_t object_table::insert(remedy_object_type_t type, remedy_handle_t owner_domain, void* resource_ptr) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t slot_idx = 0;
    if (!free_list_.empty()) {
        slot_idx = free_list_.back();
        free_list_.pop_back();
    } else {
        slot_idx = high_watermark_++;
        ensure_capacity(slot_idx);
    }

    object_slot* slot = get_slot(slot_idx);
    assert(slot != nullptr);

    if (slot->generation.load(std::memory_order_relaxed) == 0) {
        slot->generation.store(1, std::memory_order_relaxed);
    }

    slot->type.store(type, std::memory_order_relaxed);
    slot->owner_domain.store(owner_domain, std::memory_order_relaxed);
    slot->resource_ptr = resource_ptr;
    slot->pin_count.store(0, std::memory_order_relaxed);
    slot->state.store(REMEDY_SLOT_LIVE, std::memory_order_release);

    return remedy_handle_make(slot->generation.load(std::memory_order_relaxed), slot_idx);
}

bool object_table::remove(remedy_handle_t handle) {
    uint32_t slot_idx = remedy_handle_slot(handle);
    uint32_t gen = remedy_handle_generation(handle);

    object_slot* slot = get_slot(slot_idx);
    if (!slot) return false;

    // Validate generation before transition
    if (slot->generation.load(std::memory_order_relaxed) != gen) return false;

    // Atomically transition from LIVE -> REVOKING
    remedy_slot_state_t expected = REMEDY_SLOT_LIVE;
    if (!slot->state.compare_exchange_strong(expected, REMEDY_SLOT_REVOKING, std::memory_order_acq_rel)) {
        return false; // Duplicate remove or not in LIVE state
    }

    // Bounded wait for active pins to drain (using condition variable notification!)
    if (slot->pin_count.load(std::memory_order_acquire) > 0) {
        std::unique_lock<std::mutex> lock(slot->pin_mutex);
        slot->pin_cv.wait_for(lock, std::chrono::milliseconds(2000), [&] {
            return slot->pin_count.load(std::memory_order_acquire) == 0;
        });
    }

    // Free underlying resource according to type
    if (slot->resource_ptr) {
        remedy_object_type_t t = slot->type.load(std::memory_order_relaxed);
        if (t == REMEDY_OBJECT_WORKER) {
            delete static_cast<worker_object*>(slot->resource_ptr);
        } else if (t == REMEDY_OBJECT_CHANNEL) {
            delete static_cast<channel_object*>(slot->resource_ptr);
        } else if (t == REMEDY_OBJECT_DOMAIN) {
            delete static_cast<domain_object*>(slot->resource_ptr);
        }
        slot->resource_ptr = nullptr;
    }

    slot->state.store(REMEDY_SLOT_RETIRED, std::memory_order_release);

    // Bump generation (generation += 1, avoid 0)
    uint32_t next_gen = slot->generation.load(std::memory_order_relaxed) + 1;
    if (next_gen == 0) next_gen = 1;
    slot->generation.store(next_gen, std::memory_order_release);

    // Return to free list
    slot->state.store(REMEDY_SLOT_FREE, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        free_list_.push_back(slot_idx);
    }

    return true;
}

bool object_table::is_valid(remedy_handle_t handle, remedy_object_type_t expected_type) const {
    uint32_t slot_idx = remedy_handle_slot(handle);
    uint32_t gen = remedy_handle_generation(handle);

    object_slot* slot = get_slot(slot_idx);
    if (!slot) return false;

    if (slot->state.load(std::memory_order_acquire) != REMEDY_SLOT_LIVE) return false;
    if (slot->generation.load(std::memory_order_relaxed) != gen) return false;
    if (expected_type != REMEDY_OBJECT_NONE && slot->type.load(std::memory_order_relaxed) != expected_type) return false;

    return true;
}

uint32_t object_table::live_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t count = 0;
    for (uint32_t i = 1; i < high_watermark_; ++i) {
        object_slot* slot = get_slot(i);
        if (slot && slot->state.load(std::memory_order_relaxed) == REMEDY_SLOT_LIVE) {
            count++;
        }
    }
    return count;
}

} // namespace remedy
