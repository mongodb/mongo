# WASM MozJS — AOT Compilation Pipeline

This package provides build-time AOT (ahead-of-time) compilation of the MozJS WASM component so it
can be embedded directly into a binary as a pre-compiled artifact.

## Why AOT?

The raw `.wasm` component takes ~40 s to JIT-compile with wasmtime at runtime. AOT compilation does
this work once at build time, producing a serialized `.cwasm` that can be deserialized
near-instantly via `Component::deserialize()`.

## Pipeline

```
 @mozjs_wasm//file              (mozjs_wasm_api.wasm, downloaded from S3)
       │
       ▼
 wasmtime compile
       │
       ▼
 mozjs_wasm_api.cwasm           (serialized pre-compiled component)
       │
       ▼
 objcopy  (embed_mozjs_wasm_obj genrule)
       │
       ▼
 embedded_mozjs_wasm.o          (ELF .o, .rodata section)
       │
       ▼
 linked into final binary       (symbols: _binary_mozjs_wasm_api_cwasm_{start,end})
```

The AOT tool **must** be built with the same wasmtime library version and engine configuration as
the binary that will deserialize the `.cwasm`.

## Bazel Targets

| Target                    | Kind               | Output                                        |
| ------------------------- | ------------------ | --------------------------------------------- |
| `:aot_compile_mozjs_wasm` | `aot_compile_wasm` | `mozjs_wasm_api.cwasm`                        |
| `:embed_mozjs_wasm_obj`   | `genrule`          | `embedded_mozjs_wasm.o` (linkable ELF object) |

## Linking into a Binary

To embed the pre-compiled WASM module into mongod/mongos or any other binary, add the embedded
object as a linker input:

```python
mongo_cc_binary(
    name = "mongod",
    # ... existing srcs/deps ...
    deps = [
        # ... existing deps ...
        "@crates//:wasmtime_c",
    ],
    additional_linker_inputs = [
        "//src/mongo/scripting/mozjs/wasm:embed_mozjs_wasm_obj",
    ],
    linkopts = [
        "$(location //src/mongo/scripting/mozjs/wasm:embed_mozjs_wasm_obj)",
    ],
)
```

At runtime, the embedded bytes are accessible via symbols produced by objcopy:

```cpp
extern "C" {
extern const uint8_t _binary_mozjs_wasm_api_cwasm_start[];
extern const uint8_t _binary_mozjs_wasm_api_cwasm_end[];
}

// Load the pre-compiled component (near-instant).
std::vector<uint8_t> bytes(
    _binary_mozjs_wasm_api_cwasm_start,
    _binary_mozjs_wasm_api_cwasm_end);

wasmtime::Config config;
config.wasm_component_model(true);
wasmtime::Engine engine(std::move(config));

auto component = wasmtime::component::Component::deserialize(engine, bytes);
```

## Platform Support

- **Linux only** (`target_compatible_with = ["@platforms//os:linux"]`).
- Supports both `x86_64` and `aarch64` (the objcopy genrule handles both).

## Building

```bash
# Build the full pipeline (AOT compile + embed)
bazel build //src/mongo/scripting/mozjs/wasm:embed_mozjs_wasm_obj

# Build just the AOT compile tool
bazel build //src/mongo/scripting/mozjs/wasm:wasm_aot_compile_tool
```

## Testing

```bash
# Bridge Tests
bazel run +wasm_mozjs_test
# Scope Tests
bazel run //src/mongo/scripting:scripting_mozjs_test --//bazel/config:js_engine=wasm
```
