# GCC IBM builds on ARM64

This adapts SERVER-134377 and SERVER-135110 for v9.0's Bazel 7.5 and GCC 14.2. Builds execute on
ARM64; tests execute on native s390x or PPC64LE hosts.

Use a RHEL ARM64 host matching the target release:

```sh
bazel build install-dist-test --config=linux-s390x-rhel9-cross-gcc-arm64
```

The RHEL8 and RHEL10 configurations use the corresponding release in the name. Docker or Podman must
be available to extract the pinned target image filesystem. The cross archive includes GCC,
binutils, libstdc++ and libgcc for the target. The sysroot supplies target glibc, CRT files, OpenSSL
and other system libraries. Only target directories are added to the cross compiler's header and
library search paths. The native ARM64 toolchain builds tools such as protoc and Wasmtime.
Compilation and linking run on the ARM64 host; remote execution is disabled until RBE workers can
consume the new GCC archives. The target sysroot is extracted separately from the optional native
sysroot, so `--config=local` keeps native execution tools on the host's headers and libraries.

Evergreen applies the cross configuration only on ARM64 hosts. The same variants continue to compile
and execute unit tests and dbtests with native GCC on the target architecture. The native Python
environment is uploaded separately so downstream IBM tests never consume the ARM64 Python
environment. Shared variants use their own archive and environment paths. A native task records the
cross-built mongod version.

## Validation and publication

The toolchain pins use the published RHEL ARM64 archives from toolchain-builder revision
`fd8e37ded721d4d8584efd3150136c04dd793121` (PR 153), with a SHA-256 for each complete archive.

The smoke target exercises C++20, libstdc++, and 256-byte s390x alignment:

```sh
bazel build //bazel/toolchains/cc/mongo_linux_cross:cross_smoke \
  --config=linux-s390x-rhel9-cross-gcc-arm64
```

Run native jsCore against the produced archive in CI before enabling the new build hosts for
releases. Validate all three releases and the shared variants.

## PPC64LE backport

SERVER-136667 / BACKPORT-30147 uses the same GCC 14.2 / Bazel 7.5 build configuration:

```sh
bazel build install-dist-test --config=linux-ppc64le-rhel9-cross-gcc-arm64
bazel build //bazel/toolchains/cc/mongo_linux_cross:ppc64le_cross_smoke \
  --config=linux-ppc64le-rhel9-cross-gcc-arm64
```

Use the corresponding rhel8 or rhel10 configuration for those targets. The PPC compiler and runtime
pins come from the published ARM64 toolchain-builder revision
`1bce2cbf33a4c08af2e73c3b2084e282ca76f3f5` (PRs 154 and 156); no manual S3 upload is needed. PPC
builds use the ARM64-hosted LLD 19.1.7 from the pinned `@mongo_toolchain_v5` repository, with its
executables and supporting libraries declared as Bazel inputs. The newer linker bundled in the PPC
cross archive is not selected. The existing s390x linker configuration is preserved. The smoke
target verifies little-endian PPC64, 128-byte alignment, libstdc++, threading support, and the
generated config header's AltiVec byte-mask behavior. The AltiVec output lane follows the supported
GCC 14 / Clang 19 ABI, matching master without executing a target probe on the ARM64 host. PPC Rust
remains disabled by the existing v9.0 build settings. Static release artifacts build on release
ARM64 distros; shared test archives build on regular ARM64 distros. Native PPC hosts retain unit,
integration, dbtest, jsCore, and library-test coverage.

Check release linker selection and its declared executables without compiling the smoke binary:

```sh
bazel test //bazel/toolchains/cc/mongo_linux_cross:ppc_cross_lld_link_test \
  --config=opt --config=linux-ppc64le-rhel9-cross-gcc-arm64
```
