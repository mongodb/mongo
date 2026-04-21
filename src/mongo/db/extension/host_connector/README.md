# Extension Host Connector

This directory holds logic for connecting data structures from the C Public Extensions API for
idiomatic usage in C++ extension host code.

TODO SERVER-110661 Fill this in with details about handle/VTableAPI implementation.

**Purpose:** Provide C++ wrappers so host code never touches C Public API types directly; all
boundary crossing is in host_connector (and shared).

**Pattern:** Two kinds of wrappers:

- **Handles** (in `shared/handle/`): Thin wrappers around C API pointers using the `VTableAPI<T>`
  pattern and the `c_api_to_cpp_api<T>` trait. They validate vtables and expose type-safe C++
  methods. Used by both host_connector and SDK.
- **Adapters** (in `host_connector/adapter/`): Implement C API structs (e.g.
  `MongoExtensionHostPortal`) by holding C++ implementations and forwarding C callbacks into them.
  They bridge the C API boundary for host-provided services.

**Reference:** `shared/handle/handle.h` for `VTableAPI` and the trait; `host_connector/handle/` for
extension-level handles; `host_connector/adapter/` for adapters (e.g. `HostPortalAdapter`,
`HostServicesAdapter`). Most C API handles live under `shared/handle/` because **both** the host and
the C++ SDK need the same `VTableAPI` wrappers when calling across the boundary, so
`host_connector/handle` is knowingly small.
