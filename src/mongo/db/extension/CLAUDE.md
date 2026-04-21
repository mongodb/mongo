## Extensions API

The MongoDB Extensions API is a dynamic plugin system that loads shared libraries (`.so` files) into
the server at startup to provide additional aggregation stages. Extensions are developed, versioned,
and deployed independently of the server. The primary use case is moving Atlas Search stages
(`$vectorSearch`, `$search`, `$searchMeta`) out of the server codebase. Only Rust extensions are
supported in production; the C++ SDK here is for internal testing.

### Architecture

```
┌─────────────────────────┐    ┌──────────────────────────┐
│     Host Logic          │    │   Extension Code         │
│  (mongo::extension::    │    │  (Rust in prod,          │
│   host)                 │    │   C++ for tests)         │
├─────────────────────────┤    ├──────────────────────────┤
│   Host Connector        │    │       C++ SDK            │
│  (mongo::extension::    │    │  (mongo::extension::     │
│   host_connector)       │    │   sdk)                   │
├─────────────────────────┴────┴──────────────────────────┤
│              Public C API  (public/api.h)               │
│         Stable ABI - vtable-based polymorphism          │
└─────────────────────────────────────────────────────────┘
```

- **`public/`** - The C API header (`api.h`). All types and function pointer vtables crossing the
  ABI boundary. Written in C for ABI stability. Never add C++ types here.
- **`host/`** - Server-side integration: `DocumentSourceExtension`, extension loading, host
  services, unit tests. Must only use `host_connector` abstractions, never raw C API types.
  Namespace: `mongo::extension::host`.
- **`host_connector/`** - C++ wrappers for the host to safely call extension code. **Adapters** (in
  `adapter/`) wrap host C++ for extensions to call. **Handles** (in `handle/`) wrap extension C
  pointers for the host to call. Namespace: `mongo::extension::host_connector`.
- **`sdk/`** - C++ SDK for writing test extensions. Mirrors host_connector on the extension side.
  Namespace: `mongo::extension::sdk`.
- **`shared/`** - Utilities used by both host and SDK: `Handle<T, IsOwned>` template, `ByteBuf`,
  `ExtensionStatus`, `GetNextResult`.
- **`test_examples/`** - Reference test extensions. Pattern-match these when adding new extensions.

### Key Design Rules

**C API boundary (public/api.h):**

- All types crossing the boundary are C structs with a single vtable pointer
- BSONObj cannot cross - serialize to `MongoExtensionByteView`/`MongoExtensionByteBuf`
- C++ exceptions MUST NOT cross - use `MongoExtensionStatus*` (0 = OK, non-zero = error)
- Memory must be deallocated by the side that allocated it - ownership types provide `destroy()`
- Extensions export exactly one symbol: `get_mongodb_extension`

**Adapter/Handle pattern:**

- **Adapters** wrap C++ implementations and expose them through C API vtables
- **Handles** wrap C API pointers and provide type-safe C++ access to vtable functions
- Error conversion: `wrapCXXAndConvertExceptionToStatus()` (adapters) and
  `invokeCAndConvertStatusToException()` (handles)

**Layering:**

- `mongo::extension::host` must only use types from `mongo::extension::host_connector`, never from
  `public/api.h` directly
- Exception: host services connector logic (HostPortal) lives in `host/` due to server dependencies

**SDK constraints:**

- The SDK depends on `mongo/base` for BSONObj and DBException (SERVER-107651 tracks removal). Do NOT
  add new `mongo/base` usages beyond BSON and exception handling.
- In extension code, use `sdk_uassert()`, `sdk_tassert()` instead of the server's
  `uassert`/`tassert`

### Common Mistakes

- **Using server assertion macros in extension code.** Use `sdk_uassert(code, msg, cond)` /
  `sdk_tassert(code, msg, cond)` from `sdk/assert_util.h`, not the server's `uassert`/`tassert`.
- **Referencing `public/api.h` types from `host/` code.** Host logic must go through
  `host_connector/` abstractions.
- **Adding C++ types to `public/api.h`.** The public API is pure C for ABI stability.
- **Forgetting `--linkstatic=False` when building/testing extensions.** Extensions require dynamic
  linking.
- **Forgetting to add a new passthrough extension to `dist_test_extensions`** in
  `test_examples/BUILD.bazel`. If you add a `_mongo_extension` target but don't list it there, it
  won't be loaded in passthrough suites.
- **Letting BSONObj cross the C API boundary.** Serialize to `ByteView`/`ByteBuf` first, deserialize
  on the other side.
- **Letting C++ exceptions escape across the boundary.** Adapter code must catch all exceptions and
  convert to `MongoExtensionStatus*`.

### Aggregation Stage Lifecycle

Extension stages go through these phases, each modeled by a C API type:

1. **StageDescriptor** (`MongoExtensionAggStageDescriptor`) - Static factory registered at startup.
   Owns stage name and `parse()`. Lives for entire extension lifetime.
2. **ParseNode** (`MongoExtensionAggStageParseNode`) - Validates syntax, generates query shapes,
   **expands** (desugars) into resolved nodes.
3. **AstNode** (`MongoExtensionAggStageAstNode`) - Post-expansion. Provides static properties, binds
   to catalog context (namespace, UUID, explain verbosity).
4. **LogicalStage** (`MongoExtensionLogicalAggStage`) - Bound to instance context. Serialization,
   explain, optimization, distributed plan logic. Compiles to executable.
5. **ExecutableStage** (`MongoExtensionExecAggStage`) - Runtime: `open()`, `getNext()`, `reopen()`,
   `close()`.

Stage types: **source** (produce documents), **transform** (input -> output), **desugar** (expand
into pipeline of other stages during parsing).

### API Versioning

Uses MAJOR.MINOR (current: `0.1`). At startup, the host passes supported versions to
`get_mongodb_extension`; the extension negotiates a compatible version. MAJOR must match; host
minor >= extension minor. The server supports two major versions simultaneously (N and N-1). Minor
bumps add default SDK implementations; major bumps maintain frozen old API header snapshots.

### Where to Start Reading

| Task                                      | Start here                                                                                               |
| ----------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| Understand the C API contract             | `public/api.h` (read the comments, they are comprehensive)                                               |
| Understand how extensions load            | `host/load_extension.h`, `host/load_extension.cpp`                                                       |
| Understand DocumentSource integration     | `host/extension_stage.h`, `host/extension_stage.cpp`                                                     |
| Understand how stages are parsed/expanded | `host/aggregation_stage/` directory                                                                      |
| Add or modify an adapter/handle           | `host_connector/adapter/` and `host_connector/handle/`, plus `shared/handle/handle.h` for the template   |
| Write a new test extension                | `test_examples/foo.cpp` (simplest), `test_examples/desugar/add_fields_match.cpp` (desugar pattern)       |
| Understand test extension macros          | `sdk/test_extension_factory.h` (DEFAULT_PARSE_NODE, DEFAULT_EXTENSION macros)                            |
| Understand the SDK base classes           | `sdk/extension.h`, `sdk/extension_factory.h`, `sdk/aggregation_stage.h`                                  |
| Debug extension test config issues        | `buildscripts/resmokelib/extensions/README.md`, `test_examples/configurations.yml`                       |
| Understand vector search extension        | `test_examples/vector_search.cpp` (test), `test_examples/desugar/native_vector_search.cpp` (native impl) |

### Modifying the Public API (`public/api.h`)

This is the most sensitive file in the extensions system. Changes here affect ABI compatibility.

- **Adding a new vtable function:** Add it at the END of the vtable struct. Never reorder existing
  entries.
- **Adding a new type:** Follow the vtable + struct pattern. Include `destroy()` if ownership is
  transferred.
- **Never remove or rename** existing functions or types - this breaks ABI.
- **Bump the minor version** for backward-compatible additions. Bump major for breaking changes.
- When adding a new API function, you must also add: a host_connector handle method, an SDK base
  class virtual method (with default implementation for minor bumps), and adapter implementations on
  both sides.

### Building

```bash
# Build all test extensions + servers
bazel build install-dist-test

# Build just the test extensions
bazel build install-extensions

# Build a single extension (--linkstatic=False is required)
bazel build //src/mongo/db/extension/test_examples:foo_mongo_extension --linkstatic=False
```

Extensions use Bazel transitions (`bazel/transitions.bzl`) for `--allocator=system` and
`shared_archive=True`. Extensions are GPG-signed at build time.

### Unit Testing

```bash
# Run all extension API unit tests
bazel test +extensions_api_test --linkstatic=False

# Run a specific test (load_extension_test.cpp is part of extensions_api_test)
bazel test +extensions_api_test --linkstatic=False --test_arg=--gtest_filter='*LoadExtension*'
```

Host-side tests: `host/BUILD.bazel`. SDK-side tests: `sdk/tests/BUILD.bazel`.

### Integration Testing (resmoke)

**Passthrough suites** run all `jstests/extensions/` tests across topologies:

- `extensions_standalone`, `extensions_single_node`, `extensions_single_shard`,
  `extensions_sharded_cluster`, `extensions_sharded_collections`

```bash
# Run a passthrough suite
python3 buildscripts/resmoke.py run --suites=extensions_standalone --runAllFeatureFlagTests

# Run a single test
python3 buildscripts/resmoke.py run --suites=extensions_standalone --runAllFeatureFlagTests jstests/extensions/foo_and_bar_noops.js
```

**No-passthrough tests** (`jstests/noPassthrough/extensions/`) launch their own topologies:

```bash
python3 buildscripts/resmoke.py run --suites=no_passthrough --runAllFeatureFlagTests jstests/noPassthrough/extensions/your_test.js
```

Always use `--runAllFeatureFlagTests` - extension tests require `featureFlagExtensionsAPI`.

Resmoke auto-discovers `*_mongo_extension.so`, generates `.conf` files in `/tmp/mongo/extensions/`,
and passes `loadExtensions` to the server. Extension options come from
`test_examples/configurations.yml`.

### Test Extension Naming Conventions

- **Passthrough extensions** (loaded in all passthrough suites): MUST have `_mongo_extension` suffix
  (e.g., `foo_mongo_extension`). Add to first section of `dist_test_extensions` in
  `test_examples/BUILD.bazel`.
- **No-passthrough-only extensions**: MUST NOT have `_mongo_extension` suffix (e.g.,
  `vector_search_extension`). Add to second section.
- **Bad extensions** (expected to fail loading): Use `_bad_extension` suffix.

### Adding a New Test Extension

1. Create `your_stage.cpp` in `test_examples/` (or a subdirectory). Pattern-match `foo.cpp` (simple
   transform) or `desugar/add_fields_match.cpp` (desugar).
2. Use macros: `DEFAULT_PARSE_NODE(Name)`, `DEFAULT_EXTENSION(Name)`,
   `REGISTER_EXTENSION(NameExtension)`, `DEFINE_GET_EXTENSION()`.
3. If the extension needs options, add to `test_examples/configurations.yml`.
4. Add build targets to `test_examples/BUILD.bazel` using
   `signed_mongo_cc_extension_shared_library`.
5. Add the signed lib to `dist_test_extensions` (passthrough or no-passthrough section per naming
   convention).
6. Write unit tests in `host/` and/or `sdk/tests/`, updating respective `BUILD.bazel`.
7. Write integration tests in `jstests/extensions/` (passthrough) or
   `jstests/noPassthrough/extensions/` (custom topology).
