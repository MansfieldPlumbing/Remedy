#include "object_table.h"
#include <iostream>
#include <cassert>

struct dummy_resource {
    int id{42};
    std::string name{"test_resource"};
};

int main() {
    std::cout << "[TEST] Starting Object Table Conformance Test..." << std::endl;

    remedy::object_table table(16);
    assert(table.live_count() == 0);

    dummy_resource r1;
    remedy_handle_t h1 = table.insert(REMEDY_OBJECT_WORKER, REMEDY_INVALID_HANDLE, &r1);
    assert(h1 != REMEDY_INVALID_HANDLE);
    assert(remedy_handle_slot(h1) == 1);
    assert(remedy_handle_generation(h1) == 1);
    assert(table.is_valid(h1, REMEDY_OBJECT_WORKER));
    assert(!table.is_valid(h1, REMEDY_OBJECT_ARENA));

    // Test Acquisition Guard
    {
        auto lease = table.acquire<dummy_resource>(h1, REMEDY_OBJECT_WORKER);
        assert(static_cast<bool>(lease));
        assert(lease->id == 42);
        assert(lease->name == "test_resource");
    }

    // Test Handle Removal and Generation Bump
    bool removed = table.remove(h1);
    assert(removed);
    assert(!table.is_valid(h1, REMEDY_OBJECT_WORKER));

    // Re-insert into same slot -> generation should bump to 2
    dummy_resource r2;
    remedy_handle_t h2 = table.insert(REMEDY_OBJECT_WORKER, REMEDY_INVALID_HANDLE, &r2);
    assert(remedy_handle_slot(h2) == 1);
    assert(remedy_handle_generation(h2) == 2);
    assert(table.is_valid(h2, REMEDY_OBJECT_WORKER));

    // Old handle h1 must be rejected O(1)
    assert(!table.is_valid(h1, REMEDY_OBJECT_WORKER));
    {
        auto stale_lease = table.acquire<dummy_resource>(h1, REMEDY_OBJECT_WORKER);
        assert(!static_cast<bool>(stale_lease));
    }

    std::cout << "[TEST] Object Table Conformance Test PASSED!" << std::endl;
    return 0;
}
