# WASM Build Scripts

These scripts implement the WASM build steps that Bazel runs via genrules. You can run them **from
Bazel** (as usual) or **standalone** for debugging or CI. All behaviour is controlled by environment
variables; each script's header lists every variable and a short example.

---

## Run order

To build **mozjs_wasm_api.wasm** (the main WASM API module) outside Bazel, run scripts in this
order:

| Step | Script                         | Produces                              |
| ---- | ------------------------------ | ------------------------------------- |
| 1    | `build_spidermonkey_wasip2.sh` | SpiderMonkey tarball (libs + headers) |
| 2    | `extract_rust_shims.sh`        | `rust_shims.a` (from that tarball)    |
| 3    | `compile_mozjs_wasm_api.sh`    | `mozjs_wasm_api.wasm`                 |

Step 3 needs the tarball, `rust_shims.a`, the MongoDB base **linkset** response files (`.libs.rsp`
etc.), and all MozJS/WIT sources. The linkset files normally come from building
`//src/mongo/scripting/mozjs/wasm:mongo_base_linkset` in Bazel; for a fully standalone build you'd
need to copy those RSP files out of the Bazel output tree.

**Helper (not run directly):** `compile_wasi_source.sh` — compiles one C/C++ file at a time; called
by `compile_mozjs_wasm_api.sh` via `xargs`.

---

## Invocation examples

Assume you are in the **MongoDB repo root** and have a WASI SDK at `$WASI_SDK` (e.g. `/opt/wasi-sdk`
or `$HOME/wasi-sdk-24.0`). Paths below are relative to the repo root unless noted.

### 1. Build SpiderMonkey (WASI Preview 2)

Produces a tarball with `libjs_static.a`, headers, and Rust shims. Requires SpiderMonkey source
(e.g. from Bazel's external repo or a local gecko checkout) and the Rust shim sources under
`support/rust_shims/`.

```bash
cd src/mongo/scripting/mozjs/wasm

export WASI_SDK_PATH=/opt/wasi-sdk
export SPIDER_MACH_PATH=/path/to/gecko-dev/mach   # or path from Bazel execroot to mach
export RUST_SHIMS_LIB_RS=support/rust_shims/src/lib.rs
export CARGO_TEMPLATE_PATH=support/rust_shims/Cargo.toml.template
export OUTPUT=out/spidermonkey-wasip2-release.tar.gz

mkdir -p out
bash scripts/build_spidermonkey_wasip2.sh
```

### 2. Extract Rust shims from the tarball

```bash
cd src/mongo/scripting/mozjs/wasm

export TARBALL=out/spidermonkey-wasip2-release.tar.gz
export OUTPUT=out/rust_shims.a

bash scripts/extract_rust_shims.sh
```

### 3. Build mozjs_wasm_api.wasm

Requires the SpiderMonkey tarball, `rust_shims.a`, the linkset files, all MozJS/WIT sources, and the
WIT component-type object. Easiest is to run from the package dir and point at Bazel's outputs for
linkset and WIT object; fill in the source lists from the genrule in `BUILD.bazel` if needed.

```bash
cd src/mongo/scripting/mozjs/wasm

export WASI_SDK_PATH=/opt/wasi-sdk
export CXX=$WASI_SDK_PATH/bin/wasm32-wasip2-clang++
export CC=$WASI_SDK_PATH/bin/wasm32-wasip2-clang

export SM_TARBALL=out/spidermonkey-wasip2-release.tar.gz
export RUST_SHIMS_PATH=out/rust_shims.a
export WIT_COMPONENT_TYPE_OBJ=wit_gen/generated/api_component_type.o   # or path in bazel-bin

# Paths to linkset RSP files (e.g. from bazel build of :mongo_base_linkset)
export LINKSET_FILES="/path/to/xxx.objects.rsp /path/to/xxx.libs.rsp /path/to/xxx.flags.rsp"

# Header parents for error_codes and mongo config (e.g. bazel-bin dirs)
export ERROR_CODES_HEADER_FILES=/path/to/error_codes
export CONFIG_HEADER_FILES=/path/to/mongo_config

# Source file lists (space-separated); see BUILD.bazel genrule "mozjs_wasm_api_genrule" for full list
export WASM_SOURCES="engine/engine.cpp engine/error.cpp helpers.cpp ..."
export COMMON_WASI_SOURCES="/path/to/common/wasi_sources..."
export EXCEPTION_STUBS_SRC=engine/exception_stubs.cpp
export MOZJS_API_SRC=engine/api.cpp
export WIT_API_C=wit_gen/generated/api.c

export OUTPUT=out/mozjs_wasm_api.wasm
bash scripts/compile_mozjs_wasm_api.sh
```

For the exact source lists and paths, run the corresponding genrule once and inspect the `cmd` in
`BUILD.bazel`, or run the build via Bazel and use the script only for repros.

---

## What each script does

| Script                           | Purpose                                                                                                                                                              |
| -------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **build_spidermonkey_wasip2.sh** | Builds SpiderMonkey for WASI Preview 2 (mozconfig + `mach build`), builds the Rust encoding shims, and packs static libs, headers, and extra objects into a tarball. |
