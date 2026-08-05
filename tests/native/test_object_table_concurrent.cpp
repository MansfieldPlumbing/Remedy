#include "object_table.h"
#include <iostream>
#include <thread>
#include <future>
#include <cassert>

struct sync_resource {
    int value{42};
};

int main() {
    std::cout << "[TEST] Starting Deterministic Object Table Synchronization Test..." << std::endl;

    remedy::object_table table;

    // 1. Duplicate Remove Test
    sync_resource r1;
    remedy_handle_t h1 = table.insert(REMEDY_OBJECT_WORKER, REMEDY_INVALID_HANDLE, &r1);
    assert(table.is_valid(h1, REMEDY_OBJECT_WORKER));
    assert(table.remove(h1) == true);
    assert(table.remove(h1) == false); // Duplicate remove MUST return false!

    // 2. Generation Reuse & Stale Handle Test
    sync_resource r2;
    remedy_handle_t h2 = table.insert(REMEDY_OBJECT_WORKER, REMEDY_INVALID_HANDLE, &r2);
    assert(remedy_handle_slot(h2) == remedy_handle_slot(h1));
    assert(remedy_handle_generation(h2) != remedy_handle_generation(h1));
    assert(!table.is_valid(h1)); // Old handle stale

    // 3. Deterministic Pin / Remove Synchronization Test
    sync_resource r3;
    remedy_handle_t h3 = table.insert(REMEDY_OBJECT_WORKER, REMEDY_INVALID_HANDLE, &r3);

    std::promise<void> lease_pinned_promise;
    std::shared_future<void> lease_pinned_future = lease_pinned_promise.get_future().share();

    std::promise<void> remove_started_promise;
    std::shared_future<void> remove_started_future = remove_started_promise.get_future().share();

    std::promise<void> release_lease_promise;
    std::shared_future<void> release_lease_future = release_lease_promise.get_future().share();

    std::atomic<bool> remove_completed{false};
    std::atomic<bool> acquire_after_revoking_failed{false};

    // Thread 1: Acquire lease and hold pin
    std::thread holder([&]() {
        auto lease = table.acquire<sync_resource>(h3);
        assert(static_cast<bool>(lease));
        assert(lease->value == 42);

        // Signal remover that lease is pinned
        lease_pinned_promise.set_value();

        // Wait until remover enters REVOKING state and attempts remove
        remove_started_future.wait();

        // Wait for release signal
        release_lease_future.wait();

        // Release lease pin
        lease.reset();
    });

    // Thread 2: Call remove while lease is pinned
    std::thread remover([&]() {
        // Wait until holder pins lease
        lease_pinned_future.wait();

        // Signal that remove is starting
        remove_started_promise.set_value();

        // remove() will transition state to REVOKING and wait for pin_count == 0
        bool res = table.remove(h3);
        assert(res == true);
        remove_completed.store(true);
    });

    // Main thread: Verify acquire fails immediately after revocation begins
    remove_started_future.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Ensure state is REVOKING

    // Acquisition MUST fail while slot is REVOKING
    auto acquire_revoking = table.acquire<sync_resource>(h3);
    assert(!static_cast<bool>(acquire_revoking));
    acquire_after_revoking_failed.store(true);

    // Remove must NOT have completed yet because pin is still held by holder
    assert(remove_completed.load() == false);
    std::cout << "[TEST] Verified remove() is blocked waiting for active pin drain!" << std::endl;

    // Unblock holder to release pin
    release_lease_promise.set_value();

    holder.join();
    remover.join();

    assert(remove_completed.load() == true);
    assert(acquire_after_revoking_failed.load() == true);
    assert(!table.is_valid(h3));

    std::cout << "[TEST] Deterministic Object Table Synchronization Test PASSED!" << std::endl;
    return 0;
}
