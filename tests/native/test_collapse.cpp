#include "domain.h"
#include "object_table.h"
#include "revoke/ports/worker_port.h"
#include <iostream>
#include <cassert>

int main() {
    std::cout << "[TEST] Starting Domain Collapse Conformance Test..." << std::endl;

    revoke::object_table table(16);

    // 1. Create Domain
    revoke_handle_t domain_h = table.insert(REVOKE_OBJECT_DOMAIN, REVOKE_INVALID_HANDLE, nullptr);
    revoke::domain d(domain_h, &table);

    assert(d.state() == REVOKE_DOMAIN_ACTIVE);

    // 2. Attach dummy worker handle to domain
    revoke_handle_t worker_h = table.insert(REVOKE_OBJECT_WORKER, domain_h, nullptr);
    assert(table.is_valid(worker_h, REVOKE_OBJECT_WORKER));

    revoke_err_t attach_res = d.attach_worker(nullptr, worker_h);
    assert(attach_res == REVOKE_OK);

    // 3. Perform Collapse
    revoke_err_t collapse_res = d.collapse(500);
    assert(collapse_res == REVOKE_OK);
    assert(d.state() == REVOKE_DOMAIN_DEAD);

    // 4. Verify worker handle was invalidated by domain collapse
    assert(!table.is_valid(worker_h, REVOKE_OBJECT_WORKER));

    // 5. Verify Collapse is idempotent
    revoke_err_t second_collapse = d.collapse(500);
    assert(second_collapse == REVOKE_OK);
    assert(d.state() == REVOKE_DOMAIN_DEAD);

    std::cout << "[TEST] Domain Collapse Conformance Test PASSED!" << std::endl;
    return 0;
}
