# Revoke Project Status

## Target Architecture

Revoke targets a native C++ Generation Zero core that manages worker containment, identity, domain handles, IPC channels, and request lifecycle collapse while delegating managed runtimes (such as PowerShell or browser automation) to subordinate worker processes.

## Implemented Mechanisms

- Recovered baseline repository structure and initial architecture specification documents.
- Governance and contract definitions for controlled reconstruction.
- Canonical local build driver (`build.ps1`) and explicit CMake target structure (`CMakeLists.txt`).

## Verified Invariants

None independently verified in the canonical GitHub build.

## Compiled Targets & Local Build Graph

- `revoke_echo_worker` (`src/host/win32/revoke_echo_worker.cpp`) - Executable host process target.
- `test_codec_golden` (`tests/native/test_codec_golden.cpp`) - Executable test target verifying wire frame little-endian golden codec.

## Header-Only Components

- `include/revoke/envelopes.h`
- `include/revoke/handle.h`
- `include/revoke/ports/arena_port.h`
- `include/revoke/ports/channel_port.h`
- `include/revoke/ports/slot_port.h`
- `include/revoke/ports/worker_port.h`
- `include/revoke/types.h`
- `include/revoke/wire_frame.h`
- `src/native/core/core_objects.h`
- `src/native/core/object_table.h`

## Explicitly Deferred Recovered Sources

The following recovered files are explicitly excluded from the active CMake build graph pending separate, authorized semantic reconstruction:

1. `src/native/core/object_table.cpp` - Deferred. Requires `revoke::domain_object::~domain_object()` destructor implementation (missing `domain.cpp`).
2. `src/native/platform/win32/channel_win32.cpp` - Deferred. Native Win32 named pipe IPC channel port integration deferred until worker/channel subsystem task.
3. `src/native/platform/win32/worker_win32.cpp` - Deferred. Native Win32 worker process containment port integration deferred until worker subsystem task.
4. `src/managed/Revoke.EchoWorker/Program.cs` - Deferred. Managed C# worker runtime execution deferred per architecture contract.
5. `src/managed/Revoke.EchoWorker/Revoke.EchoWorker.csproj` - Deferred. Managed C# project build deferred per architecture contract.
6. `tests/native/test_collapse.cpp` - Deferred. Missing header dependency `domain.h` (domain collapse subsystem deferred).
7. `tests/native/test_echo_slice.cpp` - Deferred. Missing header dependency `domain.h` (domain host slice subsystem deferred).
8. `tests/native/test_object_table.cpp` - Deferred. Compiler error C2665 constructor signature mismatch on `revoke::object_table(int)` vs `revoke::object_table()`.
9. `tests/native/test_object_table_concurrent.cpp` - Deferred. Linker error LNK2019 unresolved external `revoke::domain_object::~domain_object()`.

## Known Gaps

- Canonical GitHub CI workflows and automated build system are not yet established.
- Generation Zero core, worker containment, and IPC channels are not yet implemented or built.
- MCP server interface (`Revoke-ServiceRequest`) is not yet operational.
- Managed runtime workers (PowerShell, browser) are not yet integrated.
