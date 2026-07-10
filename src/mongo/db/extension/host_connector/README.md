# Extension Host Connector

This directory holds C++ wrappers that let host code interact with the C Public Extensions API
(`public/api.h`) without ever touching raw C API types directly. All boundary crossing is
encapsulated here (and in `shared/`).

Two kinds of wrappers make up the host connector layer:

- **Handles** (`shared/handle/`): thin wrappers around C API pointers, built from `VTableAPI<T>` and
  `Handle<T, IsOwned>`. Used by both `host_connector` and the SDK. See **VTableAPI\<T\>** and
  **Handle\<T, IsOwned\>** below.
- **Adapters** (`host_connector/adapter/`): implement C API structs (e.g.
  `MongoExtensionHostPortal`) by holding a C++ implementation and forwarding C callbacks into it.
  Used for host-provided services. See **Adapter pattern** below.

## Background: the C API boundary

See `public/README.md` for the full rationale behind the C API's design. Here's an example struct:

```c
typedef struct MongoExtensionExecAggStage {
    const MongoExtensionExecAggStageVTable* const vtable;
} MongoExtensionExecAggStage;

typedef struct MongoExtensionExecAggStageVTable {
    MongoExtensionStatus* (*get_next)(MongoExtensionExecAggStage*, ...);
    void (*destroy)(MongoExtensionExecAggStage*);
    // ...
} MongoExtensionExecAggStageVTable;
```

## VTableAPI\<T\>

`VTableAPI<T>` (`shared/handle/handle.h`) is the base class for all C++ wrappers around C API
pointers. See the class doc comment there for its ownership contract (it does not own the pointer —
callers must guarantee the pointee outlives the `VTableAPI`) and specialization requirements.
`Handle<T, IsOwned>` (below) satisfies that contract automatically for both owned and unowned
pointers — see its `_ptr`/`_api` member order note under **Handle\<T, IsOwned\>**.

A few implementation details not spelled out in that comment:

- It has no default implementation (`= delete`) — every specialization must supply one, or the class
  fails to compile. See the `_assertValidVTable()` comment in `handle.h` for when it's invoked.
- `assertValid()` tasserts the pointer itself is non-null; `get()` has const and non-const
  overloads.

Concrete API classes subclass `VTableAPI<T>` and add named C++ methods:

```cpp
class ExecAggStageAPI : public VTableAPI<::MongoExtensionExecAggStage> {
public:
    void open() {
        invokeCAndConvertStatusToException([&] { return _vtable().open(get()); });
    }
    ExtensionGetNextResult getNext(MongoExtensionQueryExecutionContext* ctx);
    // ...

    static void assertVTableConstraints(const VTable_t& vtable) {
        tassert(ErrorCodes::InvalidExtensionVTable, "ExecAggStage 'open' is null",     vtable.open     != nullptr);
        tassert(ErrorCodes::InvalidExtensionVTable, "ExecAggStage 'get_next' is null", vtable.get_next != nullptr);
        // ...
    }
};
```

Specializations live in `shared/handle/aggregation_stage/` (e.g. `executable_agg_stage.h`) because
both the host and the C++ SDK need the same wrappers when calling across the boundary.
`host_connector/handle/` adds only the extension-level handle (`ExtensionAPI` / `ExtensionHandle`),
which is host-only.

## c_api_to_cpp_api\<T\> trait

`c_api_to_cpp_api<T>` (`shared/handle/handle.h`) is the glue between a C API type and its VTableAPI
implementation. The default maps a type to itself (no-op), and each C API type provides a
specialization:

```cpp
// Generic default (identity)
template <typename T>
struct c_api_to_cpp_api { using CppApi_t = T; };

// Specialization for ExecAggStage — maps C type → C++ API class
template <>
struct c_api_to_cpp_api<::MongoExtensionExecAggStage> {
    using CppApi_t = ExecAggStageAPI;
};
```

`Handle<T>` (below) uses this trait to pick the right VTableAPI subclass automatically.

## Handle\<T, IsOwned\>

`Handle<T, IsOwned>` (`shared/handle/handle.h`) combines ownership semantics with VTableAPI access
for a pointer allocated on the other side of the API boundary. See the class doc comment there for
the ownership model (owned via `unique_ptr` + custom deleter vs. unowned raw pointer), why it
doesn't expose the underlying `VTableAPI` except through `operator->()`, and the const-correctness
rationale.

Implementation details not spelled out in that comment:

- `ExtensionObjectDeleter::operator()` calls `extensionObj->vtable->destroy(extensionObj)` — never
  `delete` — so destruction always goes back through the vtable.
- `_ptr` is declared before the `CppApi_t` member `_api`, so members are destroyed in reverse order:
  `_api` is torn down first, and only then does `_ptr` go out of scope — which, when `IsOwned`, is
  what actually calls `vtable->destroy()`. This is what guarantees the pointee outlives the
  `VTableAPI` member even for `OwnedHandle`.
- On construction, and on copy/move when `IsOwned`, the private `_assertValidDestroy()` tasserts the
  vtable and `vtable->destroy` are non-null — so an owned handle can never outlive its ability to
  clean itself up.
- `isValid()` returns whether the underlying pointer is non-null.
- `release()` resets `_api` to a null-pointer `CppApi_t` and returns the raw pointer, giving up the
  Handle's ownership (via `unique_ptr::release()` when owned) without calling `destroy()`. Used when
  ownership is being handed back across the API boundary.
- Copy construction/assignment are conditionally enabled only when `UnderlyingPtr_t` is
  copy-constructible (via a `requires` clause) — in practice this means `OwnedHandle` (backed by a
  move-only `unique_ptr`) is move-only, while `UnownedHandle` (backed by a raw pointer) is copyable.
  Move construction/assignment on an unowned Handle also null out the source's pointer, matching
  moved-from semantics even though a raw pointer would otherwise remain valid.

Two aliases cover the common cases:

```cpp
template <typename T> using OwnedHandle   = Handle<T, true>;   // calls destroy() on scope exit
template <typename T> using UnownedHandle = Handle<T, false>;  // never destroys
```

Usage example (`shared/handle/aggregation_stage/executable_agg_stage.h`):

```cpp
using ExecAggStageHandle        = OwnedHandle<::MongoExtensionExecAggStage>;
using UnownedExecAggStageHandle = UnownedHandle<::MongoExtensionExecAggStage>;
```

The `static_assert(!std::is_same_v<T, CppApi_t>)` enforces that every `Handle<T>` instantiation has
a real VTableAPI mapping — you cannot accidentally use a type that hasn't been registered with the
trait.

Owned handles are heap-allocated with plain `new`, not `make_unique().release()`, since the
`unique_ptr` inside `Handle` is only used for its custom deleter, not the allocation itself; the
scope of the `Handle` — not the `unique_ptr` — is what drives destruction.

## Ownership at the boundary

`public/README.md` states the rule: the allocating side owns the memory, and it must be deallocated
via `destroy()` on the same side. Handle types encode that rule in C++:

| Situation                                                                        | Handle type                                                   |
| -------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| Extension allocates and returns to host (e.g. `compile()` → `ExecAggStage`)      | `OwnedHandle` — host calls `destroy()` on scope exit          |
| Host allocates and passes to extension (e.g. `HostPortal` during `initialize()`) | Passed as raw `const T*`; extension must not call `destroy()` |
| Extension receives back its own pointer (e.g. vtable callback `self` param)      | `UnownedHandle` or raw — extension owns it                    |

## Full call chain: host calls extension `getNext()`

This traces a single `getNext()` call from host C++ through the C boundary and back.

```
Host C++:
  handle->getNext(execCtx)
        │ (operator-> returns ExecAggStageAPI*)
        ▼
  ExecAggStageAPI::getNext()
    calls invokeCAndConvertStatusToException([&] {
        return _vtable().get_next(get(), execCtxPtr, &result);
    })
        │ (_vtable() dereferences the extension's vtable pointer)
        ▼
─── C ABI boundary ────────────────────────────────────────────────────────
        │
        ▼
  Extension vtable entry: _extGetNext() (static, noexcept)
    return wrapCXXAndConvertExceptionToStatus([&]() {
        impl->getNext(ctx, result);  // extension C++ code; may throw
    });
        │ (returns MongoExtensionStatus* to host)
─── C ABI boundary ────────────────────────────────────────────────────────
        │
        ▼
  invokeCAndConvertStatusToException checks status:
    • OK  → continue normally
    • err → convertStatusToException() → throws C++ exception
```

Files involved in this chain:

- `public/api.h` — `MongoExtensionExecAggStage` and its vtable struct
- `shared/handle/aggregation_stage/executable_agg_stage.h` — `ExecAggStageAPI` and handle aliases
- `shared/extension_status.h` — `wrapCXX...` and `invokeC...`
- Extension adapter (`sdk/aggregation_stage.h` for C++ SDK, Rust SDK for production) — provides
  `_extGetNext`

## Adapter pattern (host → extension direction)

When the host provides services to extensions (e.g. `MongoExtensionHostPortal` during
`initialize()`), it exposes C++ implementations through the C API using **adapters** in
`host_connector/adapter/`. An adapter:

1. **Subclasses the C API struct directly** so a pointer to it _is_ a valid C API pointer:
   ```cpp
   class HostPortalAdapter final : public ::MongoExtensionHostPortal { ... };
   ```
2. **Initializes the vtable with a static constant** pointing to static C functions:
   ```cpp
   HostPortalAdapter(...) : ::MongoExtensionHostPortal{&VTABLE, apiVersion, maxWireVersion}, ...
   ```
3. **Holds the C++ implementation** via an owning pointer:
   ```cpp
   std::unique_ptr<HostPortalBase> _portal;
   ```
4. **Each vtable static function** casts the opaque C API pointer back to `HostPortalAdapter*`, then
   delegates to the C++ implementation wrapped in `wrapCXXAndConvertExceptionToStatus()`:
   ```cpp
   static MongoExtensionStatus* _extRegisterStageDescriptor(
           const MongoExtensionHostPortal* p,
           const MongoExtensionAggStageDescriptor* desc) noexcept {
       return wrapCXXAndConvertExceptionToStatus([&]() {
           static_cast<const HostPortalAdapter*>(p)->getImpl().registerStageDescriptor(desc);
       });
   }
   ```

The extension calls the C API completely unaware that it is backed by a C++ object.

## File map

| File                                       | Role                                                                       |
| ------------------------------------------ | -------------------------------------------------------------------------- |
| `public/api.h`                             | C API: all vtable structs and function pointer types                       |
| `shared/handle/handle.h`                   | `VTableAPI<T>`, `Handle<T,IsOwned>`, `c_api_to_cpp_api<T>`                 |
| `shared/extension_status.h`                | `wrapCXXAndConvertExceptionToStatus`, `invokeCAndConvertStatusToException` |
| `shared/handle/aggregation_stage/*.h`      | VTableAPI specializations for each aggregation stage C type                |
| `host_connector/handle/extension_handle.h` | `ExtensionAPI` / `ExtensionHandle` for `MongoExtension`                    |
| `host_connector/adapter/*.h`               | Adapters exposing host C++ implementations as C API vtables                |
