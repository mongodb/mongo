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
a particular Docker-compatible runtime. Builds fail closed if neither runtime is available, the
selected runtime cannot start the pinned image, or the image cannot be pulled. Only the explicit
opt-out above allows native build-tool execution. Existing native behavior is retained, with a
warning, when:

- the host distro has no pinned RBE container or no mongo toolchain for the host architecture,
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
under `<output-base>/mongo_linux_action_tmp/`; failed local actions report the container UID and the
ownership/access modes of their cwd, temporary directory, repository, and output base.

The output base is mounted read-only, while Bazel's persistent-container worker directory and
process sandboxes use the separate read/write sibling `<output-base>-mongo-action-sandbox`. Avoiding
nested bind mounts makes the layout independent of Docker's mount ordering. Per-configuration
`bazel-bin/install` trees are stored below the separate `<output-base>-mongo-shared-install` mount
and the active one is published through its traditional workspace symlink after the build. Before
every invocation, the wrapper verifies the read-only output mount and performs real writes through
the temporary, sandbox, and shared-install mounts. A stale or incorrectly mounted persistent
container is removed and recreated once automatically; if the replacement fails the same checks, the
build fails closed before any action runs. The current v5 layout is required; configs from earlier
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

For `s390x` and `ppc64le` RBE cross builds, use the explicit host-Bazel configs:

```bash
bazel build --config=linux-s390x-cross-rbe install-dist-test
bazel build --config=linux-ppc64le-cross-rbe install-dist-test
```

Those configs keep Bazel on the host and route C++ compile, link, archive, debug-info, and IDL
actions to Linux RBE. `TestRunner` actions stay standalone on the host, so these configs are meant
for native `s390x` or `ppc64le` hosts that can execute the resulting binaries after remote build
artifacts are downloaded.

The default target distro is `rhel9` and the default RBE execution pool is `x86_64`. Explicit
variants are available when needed:

```bash
bazel build --config=linux-s390x-rhel8-cross-rbe install-dist-test
bazel build --config=linux-ppc64le-rhel9-cross-rbe-arm64 install-dist-test
```

Cross-RBE requires real cross toolchain archives. Configure them with URL/SHA environment variables
before running the cross configs:

```bash
export MONGO_LINUX_CROSS_TOOLCHAIN_RHEL9_S390X_ON_RHEL9_X86_64_URL=https://.../toolchain.tar.gz
export MONGO_LINUX_CROSS_TOOLCHAIN_RHEL9_S390X_ON_RHEL9_X86_64_SHA256=<sha256>
```

Target-only variables such as `MONGO_LINUX_CROSS_TOOLCHAIN_RHEL9_S390X_URL` and
`MONGO_LINUX_CROSS_TOOLCHAIN_RHEL9_S390X_SHA256` can be used when the same archive works for every
execution arch. The archive must contain compiler, linker/binutils, sysroot, runtime libraries, and
toolchain files that execute on the selected RBE worker architecture while producing the requested
`s390x` or `ppc64le` Linux target artifacts.

Use `MONGO_LINUX_CROSS_RBE_CONTAINER_IMAGE` or `MONGO_LINUX_CROSS_RBE_POOL` only for debugging or
one-off RBE experiments; the checked-in configs otherwise use the pinned `rhel9` RBE container and
the matching EngFlow pool.
