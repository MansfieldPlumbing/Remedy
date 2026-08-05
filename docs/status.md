# Remedy Project Status

## Target Architecture

Remedy targets a native C++ Generation Zero core that manages worker containment, identity, domain handles, IPC channels, and request lifecycle collapse while delegating managed runtimes (such as PowerShell or browser automation) to subordinate worker processes.

## Implemented Mechanisms

- Recovered baseline repository structure and initial architecture specification documents.
- Governance and contract definitions for controlled reconstruction.

## Verified Invariants

None independently verified in the canonical GitHub build.

## Known Gaps

- Canonical GitHub CI workflows and automated build system are not yet established.
- Generation Zero core, worker containment, and IPC channels are not yet implemented or built.
- MCP server interface (`Remedy-ServiceRequest`) is not yet operational.
- Managed runtime workers (PowerShell, browser) are not yet integrated.
