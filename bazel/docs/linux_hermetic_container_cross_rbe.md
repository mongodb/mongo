# Linux Host Container Builds And s390x/ppc64le Cross RBE

## Host Container Mode

Normal Linux builds keep Bazel on the host and run only locally-executed build actions inside a
persistent local container started from the same pinned RBE image the host platform advertises
(`bazel/platforms/remote_execution_containers.bzl`). This is also the default on supported Linux CI
hosts, including Evergreen. It mirrors the macOS/Windows host-cross setups, minus the cross
toolchain:

- The Bazel client/server, repository fetches, workspace status, and `bazel run` execute natively on
  the host. Test actions retain their normal Bazel strategy: `--config=remote_test` runs them on
  RBE, while `--config=local` runs them natively on the host.
- Every repository-provided tool spawn is containerized: compilers, linkers, Rust and Cargo tools,
  Python generators and workers, IDL/proto generation, signing, packaging, install assembly,
  genrules, and WASM/WIT tools. Bazel-internal writes, symlinks, manifests, template expansion,
  repository fetching, and analysis remain native.
- Link, archive, DWP, strip, and local-only install actions stay local by default (matching the
  strategies in `.bazelrc`) and run inside the container.
- C++ and Rust compiler actions use Bazel dynamic scheduling when remote execution is enabled: each
  compile races Linux RBE against the local container, and the first branch to finish wins. Because
  the local branch runs in the same pinned image as the remote workers, local and remote results are
  identical. Dynamic local execution is limited to one eighth of the host CPUs and half of host RAM,
  independently of the larger `--jobs` value used to feed RBE. Bazel's input-based C++ resource
  estimator and dynamic local load control are enabled as additional safeguards. Set
  `MONGO_LINUX_DYNAMIC_SCHEDULING=0` to keep compiles purely remote.
- With `--config=local` (remote execution disabled), repository-provided build tools run locally
  through the container. `TestRunner`, coverage processing, and programs launched by `bazel run`
  stay native.

The wrapper hook writes a `mongo_linux_container_actions.json` file into the Bazel output base
before each build. Bazel's persistent-container spawn strategy launches every locally executed build
action through the already-running container, while remote actions continue to execute on RBE
unchanged. This means ordinary `ctx.actions.run` and upstream toolchains work in both paths; no
per-rule wrappers, module overrides, or third-party action patches are required.

The Rust compiler, Cargo tools, and standard library are still Bazel-managed `rules_rust` toolchain
inputs. They are mounted from the execroot and are not installed in the image.

To explicitly allow repository build tools to run natively on the host:

```bash
MONGO_LINUX_CONTAINER_ACTIONS=0 bazel build install-dist-test
```

or use `MONGO_BAZEL_USE_HERMETIC_CONTAINER=0`, which also bypasses the rest of the wrapper-hook
routing. Supported Linux distro/architecture combinations use Docker when available and otherwise
fall back to Podman. If `docker` is Podman's compatibility shim, the wrapper uses the real `podman`
command instead, so that rootless Podman uses `--userns=keep-id` and disables SELinux label
separation for the repository/output bind mounts rather than relabeling those host trees. Both
runtimes use the same pinned OCI image. Set `HERMETIC_CONTAINER_DOCKER_COMMAND` explicitly to force
a particular Docker-compatible runtime. Builds fall back to a native, non-containerized build if
neither runtime is available, the selected runtime cannot start the pinned image, or the image
cannot be pulled. Detection that Bazel is already running inside a container is an additional
condition that allows native build-tool execution. Set `MONGO_BAZEL_USE_HERMETIC_CONTAINER=1` to
explicitly enable nested container execution. Existing native behavior is also retained when:

- the host distro has no pinned RBE container or no mongo toolchain for the host architecture,
- Bazel is already running inside a container (unless nested container execution is explicitly
  enabled),
- the invocation uses a macOS cross configuration, which retains its original setup for now,
- the invocation is a non-build command (`query`, `clean`, `info`, ...).

Remote builds do not need the local container for any remote work: remote actions run in the pinned
image on the RBE workers. The container is only used for actions that execute on the host. With
`--config=remote_link` the link-family actions move to RBE as well.

The persistent-container strategy does not alter compiler tool paths or action inputs. Toggling
`MONGO_LINUX_CONTAINER_ACTIONS` still selects between containerized and native local execution, but
remote action keys remain independent of the host-side launcher.

The wrapper starts and validates the persistent container before Bazel begins. The container is
named `mongo_linux_action_<distro>_<arch>_<imagehash>_...`, remains running after Bazel exits, and
is reused by later invocations with the same workspace, output base, and image. It is replaced
automatically when the pinned image changes. Each action receives an isolated temporary directory
under the writable sibling `<output-base>-mongo-action-tmp/`; failed local actions report the
container UID and the ownership/access modes of their cwd, temporary directory, repository, and
output base.

The output base is mounted read-only, while Bazel's persistent-container worker directory and
process sandboxes use the separate writable sibling `<output-base>-mongo-action-sandbox`. Avoiding
nested bind mounts makes the layout independent of Docker's mount ordering. Per-configuration
`bazel-bin/install` trees are stored below the separate `<output-base>-mongo-shared-install` mount
and the active one is published through its traditional workspace symlink after the build. Before
every invocation, the wrapper verifies the read-only output mount and performs real writes through
the temporary, sandbox, and shared-install mounts. A stale or incorrectly mounted persistent
container is removed and recreated once automatically; if the replacement fails the same checks, the
build fails closed before any action runs. The current v6 layout is required; configs from earlier
layouts are rejected and regenerated by the wrapper hook on the next normal Bazel invocation.

`MONGO_HERMETIC_CONTAINER_DISTRO` overrides the distro selection for debugging. In Linux host
container mode, `MONGO_HERMETIC_CONTAINER_IMAGE` must be an immutable `docker://...@sha256:<digest>`
reference so dynamic local and remote actions use the same image.

## Verification

The boundary fixture combines a Rust library, proc macro, Cargo build script, Python generator,
protobuf generation, and a Rust test. Its host-only sentinel must be invisible during construction
and visible to the native `TestRunner` when remote execution is disabled:

```bash
bazel run //bazel/toolchains/cc/mongo_linux/testdata/container_boundary:run_boundary_test -- --config=local
```

## s390x/ppc64le Cross RBE

For `s390x` and `ppc64le` RBE cross builds, use the host-Bazel configs:

```bash
bazel build --config=linux-s390x-cross-rbe install-dist-test
bazel build --config=linux-ppc64le-cross-rbe install-dist-test
```

The generic configs select a RHEL 9 target and RHEL 9 `x86_64` execution workers. Their ARM64
counterparts select the same target with RHEL 9 `arm64` workers:

```bash
bazel build --config=linux-s390x-cross-rbe-arm64 install-dist-test
bazel build --config=linux-ppc64le-cross-rbe-arm64 install-dist-test
```

Concrete configs use the following naming scheme:

```text
linux-<s390x|ppc64le>-<rhel8|rhel9|rhel10>-cross-rbe[-arm64]
```

Cross-only protobuf and gRPC execution-platform rule changes are kept as patch files under
`bazel/third_party`. For build, test, coverage, and run commands the wrapper applies those patches
to content-addressed copies below the Bazel output base and passes them with `--override_module`;
the checked-in `src/third_party/*/dist` source distributions are never modified. Release-local and
native builds do not create these overlays.

Omitting `-arm64` selects `x86_64` execution. The target distro and execution architecture are
independent, so all RHEL 8, 9, and 10 target sysroots can be used with either RHEL 9 execution pool.
For example:

```bash
bazel build --config=linux-s390x-rhel8-cross-rbe install-dist-test
bazel build --config=linux-ppc64le-rhel9-cross-rbe-arm64 install-dist-test
bazel build --config=linux-s390x-rhel10-cross-rbe install-dist-test
bazel build --config=linux-ppc64le-rhel10-cross-rbe-arm64 install-dist-test
```

These configs keep the Bazel client and repository setup on the host. In patch builds and waterfall
tasks marked `compiling_for_test`, C++/Rust compilation, LTO compilation, IDL generation,
execution-platform genrules/wheel build and installation, and WASI tools use RBE and the selected
RHEL 9 execution architecture. Final C++ links, archives, debug extraction/stripping, DWP/GDB index
generation, packaging, and tests run in the native host action container. IBM cross mode is
deliberately compile-only; `remote_test` or `remote_link` cannot re-enable remote target links.
Local cross-output actions are marked no-cache, which avoids sending multi-gigabyte debug files
through CAS.

When `--config=opt_profiled` is selected, the IBM Clang toolchains also run distributed ThinLTO.
Bazel resolves the LTO indexing tool from the cpp-link-\* action configs, so indexing shares the
host-native linker and stays in the native IBM container beside the link; the lto-backend codegen
actions use the execution-architecture compiler and follow the remote compile routing. BOLT remains
disabled for PPC64LE and s390x until target profiles and an IBM-compatible post-link pipeline are
available.

Waterfall release/provenance tasks use `public-release-local`: the wrapper resets the foreign
execution platform/toolchain and cross repository selectors, clears remote execution, and marks all
actions `no-cache` while retaining the gRPC cache endpoint required by Bazel's remote downloader. It
uses the host-native `mongo_toolchain_v5` inside the host-native hermetic container. This keeps
release execution logs free of remote runners and cache hits. Top-level target artifacts are
downloaded, and `TestRunner` remains standalone on the host, so running target binaries still
requires a native `s390x` or `ppc64le` host. Standalone cross tests prepend the target toolchain's
`libstdc++` directories to `LD_LIBRARY_PATH`, so the host's older C++ runtime cannot override the
one used to link the target binary. The execution architecture suffix describes the RBE workers, not
the machine running the resulting binaries.

PPC target links also pass `-static-libstdc++` through the cross toolchain for executables and
shared libraries. This prevents a target `_solib` from retaining a `libstdc++.so.6` dependency that
would otherwise resolve to an older IBM host runtime during Evergreen's version check.

Tool-link actions (shown by Bazel as `CppLink [for tool]`), foreign-tool `Genrule`, `WheelBuild`,
and `WheelInstall` actions, IDL generation, and WASI compile/link actions use the execution-platform
toolchain and remain remote even when the final target link is local. `WitBindgenC` and
`RustWasmBindgen` also remain remote because their output is architecture-independent. For s390x,
`WasmAotCompile` instead runs a target-built Wasmtime CLI in the native IBM container. The CLI uses
the s390x Cranelift backend; its Rust compilation stays remote, while
`experimental_use_cc_common_link` separates its native C++ link from that compile action. Native
s390x builds retain the original execution-platform CLI. PPC64LE remains unsupported by the WASM
engine, as it was before cross-RBE. The host-side `PyWriteBuildData` tar metadata action is
explicitly local because rules_pkg marks it `no-remote`. The WASI SDK repository uses the explicit
`MONGO_WASI_SDK_EXEC_ARCH` execution override during bootstrap and hydration, with the Linux cross
selector as a compatibility fallback. Thus an IBM host does not send an IBM WASI binary to an
`x86_64` or ARM RBE worker. Native IBM WASI archives remain available when the build is not
cross-RBE, and the SDK's `llvm-ar` dispatches on the executing machine so the same hydrated
repository serves both remote and local archive actions. Evergreen gives each task an isolated
rootless Podman storage/runtime directory, resets it before use, and removes all task-owned
containers and files in task teardown. A failed Podman cleanup is reported but never hides the build
result.

The `//buildscripts:archive_artifacts` packaging binary is constrained to the invoking host's
execution platform. This matters for `bazel run` after a cross build: the archive launcher runs on
the native IBM host, so it must use the host Python rather than the Python runtime selected for the
foreign RBE execution platform. The concrete cross configs also set `MONGO_WASI_SDK_EXEC_ARCH` in
`.bazelrc`, and `tools/bazel` forwards it both on the original Bazel argument vector and during the
early `@py_host` bootstrap, before the wrapper hook has a chance to add its normal cross-RBE
arguments.

### Default pinned toolchain composition

No cross-toolchain URL or SHA is required for normal use. Each concrete config selects one
repository that composes three pinned inputs:

- the RHEL 9 `x86_64` or `aarch64` toolchain archive containing Clang, LLD, and the tools that run
  on the RBE worker;
- the matching RHEL 8, 9, or 10 `s390x` or `ppc64le` toolchain archive containing target C/C++
  headers, CRT objects, and runtime libraries; and
- a target-architecture sysroot exported from the matching pinned, multi-architecture RBE container
  image.

The compiler receives the MongoDB target triple and the extracted target sysroot, while executable
compiler tools come only from the selected RHEL 9 execution archive. Rust selects the corresponding
target standard library and uses the same cross linker. The archives, container digests, execution
platform, and EngFlow pool are selected by checked-in configuration, which keeps remote actions
independent of the host's installed compiler and system libraries.

Preparing the sysroot requires Docker or Podman on the Bazel host. The repository rule pulls the
requested `linux/s390x` or `linux/ppc64le` image variant, exports only its header and library trees,
and makes those files declared inputs to remote actions.

### Optional complete-archive override

For toolchain development or one-off experiments, a complete cross-toolchain archive can replace the
pinned composition. Set both URL and SHA variables for the exact target/execution pair:

```bash
export MONGO_LINUX_CROSS_TOOLCHAIN_RHEL10_PPC64LE_ON_RHEL9_AARCH64_URL=https://.../toolchain.tar.gz
export MONGO_LINUX_CROSS_TOOLCHAIN_RHEL10_PPC64LE_ON_RHEL9_AARCH64_SHA256=<sha256>
bazel build --config=linux-ppc64le-rhel10-cross-rbe-arm64 install-dist-test
```

Target-only variables such as `MONGO_LINUX_CROSS_TOOLCHAIN_RHEL9_S390X_URL` and
`MONGO_LINUX_CROSS_TOOLCHAIN_RHEL9_S390X_SHA256` can be used when the same archive works for every
execution architecture. An optional matching `_STRIP_PREFIX` variable strips an archive prefix.
Pair-specific variables take precedence over target-only variables.

The override is a complete replacement, not an additional target-runtime layer. It must contain the
compiler, linker/binutils, target sysroot, headers, CRT objects, runtime libraries, and toolchain
files. Its executables must run on the selected RHEL 9 RBE worker architecture while producing
artifacts for the requested `s390x` or `ppc64le` target.

Use `MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE` or `MONGO_LINUX_CROSS_RBE_POOL` only for debugging or
one-off RBE experiments; the checked-in configs otherwise use the pinned `rhel9` RBE container and
the matching EngFlow pool.
