# GCC IBM builds on ARM64

This adapts SERVER-134377 and SERVER-135110 for v8.3's Bazel 7.5 and GCC 14.2. Builds execute on
ARM64; tests execute on native s390x or PPC64LE hosts.

Use a RHEL ARM64 host matching the target release:

```sh
bazel build install-dist-test --config=linux-s390x-rhel9-cross-gcc-arm64
```

The RHEL8 and RHEL10 configurations use the corresponding release in the name. Docker or Podman must
be available to extract the pinned s390x image filesystem. The cross archive includes GCC, binutils,
libstdc++ and libgcc for the target. The sysroot supplies target glibc, CRT files, OpenSSL and other
system libraries. Only target directories are added to the cross compiler's header and library
search paths. Cross-GCC 14.2 and target runtimes come from the newer archive; linking uses
LLD 19.1.7 from v8.3's existing ARM64 v5 toolchain. Its linker and shared libraries are declared
as action inputs. The native ARM64 toolchain also builds tools such as protoc. Compilation and
linking run on the ARM64 host; remote execution is disabled until RBE workers can consume the new
GCC archives. The target sysroot is extracted separately; native execution tools keep using the host's headers and libraries.

Evergreen applies the cross configuration only on ARM64 hosts. The same variants continue to compile
and execute unit tests and dbtests with native GCC on the target architecture. The native Python
environment is uploaded separately so downstream IBM tests never consume the ARM64 Python
environment. Shared variants use their own archive and environment paths. A native task records the
cross-built mongod version.

## Validation and publication

The toolchain pins use the published RHEL ARM64 archives from toolchain-builder revision
`fd8e37ded721d4d8584efd3150136c04dd793121` (PR 153), with a SHA-256 for each complete archive.

The smoke targets exercise C++20, libstdc++, 256-byte s390x alignment, and ARM64 build-time
Python with an s390x target platform. The C++ smoke binary links a separate library to check
ARM64 LLD's handling of `--start-lib` for s390x objects. Use the release configuration to select
LLD (development configurations explicitly select mold):

```sh
bazel build //bazel/toolchains/cc/mongo_linux_cross:cross_smoke \
  //bazel/toolchains/cc/mongo_linux_cross:cross_linker_smoke \
  //bazel/toolchains/cc/mongo_linux_cross:s390x_target_platform_check \
  --config=opt \
  --config=linux-s390x-rhel9-cross-gcc-arm64
```

Run native jsCore against the produced archive in CI before enabling the new build hosts for
releases. Validate all three releases and the shared variants.

## PPC64LE backport

SERVER-136667 / BACKPORT-30148 uses the same GCC 14.2 / Bazel 7.5 build configuration:

```sh
bazel build install-dist-test --config=linux-ppc64le-rhel9-cross-gcc-arm64
bazel build //bazel/toolchains/cc/mongo_linux_cross:ppc64le_cross_smoke \
  --config=linux-ppc64le-rhel9-cross-gcc-arm64
```

Use the corresponding rhel8 or rhel10 configuration for those targets. The PPC compiler and runtime
pins come from the published ARM64 toolchain-builder revision
`1bce2cbf33a4c08af2e73c3b2084e282ca76f3f5` (PRs 154 and 156); no manual S3 upload is needed.
Both PPC and s390x links use v8.3's pinned ARM64-hosted LLD 19.1.7 from
`@mongo_toolchain_v5`, preserving the native release linker version. The newer LLD bundled
with the GCC cross archive is not selected. The
smoke target verifies little-endian PPC64, 128-byte alignment, libstdc++, threading support, and the
generated config header's AltiVec byte-mask behavior. The AltiVec output lane follows the supported
GCC 14 / Clang 19 ABI, matching master without executing a target probe on the ARM64 host. PPC Rust
remains disabled by the existing v8.3 build settings. Static release artifacts build on release
ARM64 distros; shared test archives build on regular ARM64 distros. Native PPC hosts retain unit,
integration, dbtest, jsCore, and library-test coverage.
