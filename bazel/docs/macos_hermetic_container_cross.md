# macOS Cross-Builds

This mode is for macOS hosts that want Bazel to produce macOS artifacts with the hermetic macOS
cross toolchain. Bazel runs on the macOS host, C++ compile and platform-independent generated-source
actions default to Linux RBE, C++ link/archive/debug-info actions default to a local Linux container
wrapper, and tests run on the macOS host in the same Bazel invocation.

## Usage

From a macOS shell that can run `tools/bazel`:

```bash
MONGO_MACOS_CROSS_DEFAULT_CONFIG=1 bazel build install-dist-test
```

macOS builds use the normal native setup by default. Cross-build mode is enabled by the opt-in above
or by an explicit `--config=macos-cross-*` option. To opt in to automatic config selection for
`bazel build` and `bazel test`, set:

```bash
export MONGO_MACOS_CROSS_DEFAULT_CONFIG=1
```

The wrapper then selects `--config=macos-cross-arm64` on Apple Silicon hosts and
`--config=macos-cross-x86_64` on Intel hosts. To force a specific target architecture:

```bash
bazel build --config=macos-cross-arm64 install-dist-test
bazel build --config=macos-cross-x86_64 install-dist-test
```

For build-oriented commands, the Bazel process remains on the macOS host. The macOS cross C++
toolchain invokes tools through wrappers that execute the real Linux tool directly when the action
runs on Linux RBE, and use `bazel/toolchains/cc/mongo_apple_cross/macos_cross_action_wrapper.py` to
enter a persistent Linux container when the action runs locally on macOS. Platform-independent IDL
generation uses the Linux hermetic Python runtime because the generated output is the same for macOS
and Linux.

`TestRunner` actions remain local/standalone on macOS. Remote linking is available only when
explicitly requested with `--config=remote_link`; it is not the default because some monolithic
MongoDB `.lo` outputs can exceed the RBE service's per-output size limit.

`MONGO_BAZEL_USE_HERMETIC_CONTAINER=1` keeps build/test commands in the host-cross integration path;
it does not force the whole Bazel invocation into HermeticContainer on macOS. The older two-phase
bridge is still available for tests with `MONGO_MACOS_CROSS_SPLIT_TEST_RUNNER=1`.

The container `HOME` is `.tmp/hermetic_container/home`, which gives repository tools a writable
place for caches such as `.shiv` without mounting the whole host home directory.

The hermetic_container container image is selected from the same pinned
`bazel/platforms/remote_execution_containers.bzl` mapping used by Bazel execution platforms. The
default fallback is the Amazon Linux 2023 RBE image when the host distro cannot be matched.

On macOS and Windows hosts, the wrapper generates a small local Dockerfile under
`.tmp/hermetic_container` that uses the pinned RBE image as its base and installs repository-fetch
tools such as `git`, `tar`, and `python3`. Bazel repository rules may invoke these tools while
running inside the container, so this local layer keeps the pinned base image while making those
fetches work. Set `MONGO_HERMETIC_CONTAINER_GIT_LAYER=0` to disable this layer for debugging.

## Local Test Execution

On macOS, `bazel test` uses one host Bazel invocation:

```bash
bazel test +some_test
```

The wrapper injects the Linux RBE container execution properties, explicitly keeps C++ compile and
IDL generation remote, keeps link/archive/debug-info actions local through the container wrapper,
and uses standalone/local strategies for `TestRunner`. Native test executables and resmoke suite
targets run on the macOS host in that same Bazel invocation.

The wrapper also injects `--local_resources=cpu=HOST_CPUS` and `--local_test_jobs=HOST_CPUS`. This
lets commands keep a high `--jobs` value for RBE while capping local link and test concurrency to
the host CPU count. Override those defaults with `MONGO_MACOS_CROSS_LOCAL_CPU_RESOURCES` and
`MONGO_MACOS_CROSS_LOCAL_TEST_JOBS`.

Actions that cannot run on RBE use the local Linux container wrapper. Platform-independent generated
source actions such as `IdlcGenerator` run through Linux RBE. The IDL wrapper is a portable
`sh_binary` and uses the RBE container's native `python3` on Linux so it is not tied to a specific
worker architecture. `TestRunner` actions still run on the macOS host.

The default RBE path checks Docker before starting Bazel because link/archive/debug-info actions are
expected to run through the local container wrapper. The check times out instead of waiting
indefinitely. If `--config=remote_link` is explicitly requested, the wrapper skips that Docker
preflight because all build actions are expected to run on RBE.

To disable RBE for a cross command, use one of the local configs:

```bash
bazel test --config=local +some_test
```

To force link/archive/debug-info actions onto RBE, use:

```bash
bazel test --config=remote_link +some_test
```

The normal wrapper `+source_or_test` shortcuts still apply before this routing happens, so source
file filters are preserved through `--test_arg=--fileNameFilter`. To disable the host test runner
behavior for a command, set:

```bash
MONGO_MACOS_CROSS_TEST_RUNNER=0 bazel test +some_test
```

To force the older two-phase bridge, set:

```bash
MONGO_MACOS_CROSS_SPLIT_TEST_RUNNER=1 bazel test +some_test
```

## Toolchain Inputs

By default, the macOS cross toolchain downloads pinned hermetic inputs from S3:

```text
LLVM 22.1.1
MacOSX15.2.sdk
```

That default does not use the host Xcode SDK. If you need to override the toolchain inputs, provide
Linux-executable LLVM and a macOS SDK path:

```bash
export LLVM_PATH=/path/to/llvm-cross-linux-arm64
export MACOS_SDK_PATH=/path/to/MacOSX15.2.sdk
export MACOS_MIN_VERSION=14.0
bazel build install-dist-test
```

`SDKROOT` is intentionally not passed through to hermetic_container by default. Use `MACOS_SDK_PATH`
when overriding the SDK.

## Notes

Set `MONGO_MACOS_CROSS_DEFAULT_CONFIG=1` to opt in to automatic macOS cross config selection. Set
`MONGO_MACOS_CROSS_ACTION_WRAPPER=0` to disable the local container action wrapper path. Set
`MONGO_BAZEL_USE_HERMETIC_CONTAINER=0` to bypass hermetic_container and wrapper-hook routing
entirely.

`bazel run` only infers a macOS cross config for `_test` targets. Use `bazel test` when you want
cross-build actions routed through RBE/local container strategies and the test process on the macOS
host.

## Troubleshooting

The Docker CLI being installed is not enough; the Docker daemon must also be running. On macOS,
start Docker Desktop and wait for this command to succeed before retrying a container-backed build:

```bash
docker info
```

If Bazel reports missing or expired EngFlow credentials from the Linux helper under
`.tmp/hermetic_container/engflow_auth`, refresh credentials from the macOS host and retry:

```bash
bazel run engflow_auth
```
