# GCC s390x builds on ARM64

This adapts SERVER-134377 and SERVER-135110 for v9.0's Bazel 7.5 and GCC 14.2. Builds execute on
ARM64; tests execute on native s390x hosts.

Use a RHEL ARM64 host matching the target release:

```sh
bazel build install-dist-test --config=linux-s390x-rhel9-cross-gcc-arm64
```

The RHEL8 and RHEL10 configurations use the corresponding release in the name. Docker or Podman must
be available to extract the pinned s390x image filesystem. The cross archive includes GCC, binutils,
libstdc++ and libgcc for the target. The sysroot supplies target glibc, CRT files, OpenSSL and other
system libraries. Only target directories are added to the cross compiler's header and library
search paths. The native ARM64 toolchain builds tools such as protoc and Wasmtime. Compilation and
linking run on the ARM64 host; remote execution is disabled until RBE workers can consume the new
GCC archives. The target sysroot is extracted separately from the optional native sysroot, so
`--config=local` keeps native execution tools on the host's headers and libraries.

Evergreen applies the cross configuration only on ARM64 hosts. The same variants continue to compile
and execute unit tests and dbtests with native GCC on s390x. The native Python environment is
uploaded separately so downstream s390x tests never consume the ARM64 Python environment. Shared
variants use their own archive and environment paths. A native task records the cross-built mongod
version.

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
