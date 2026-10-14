# GCC s390x builds on ARM64

This adapts SERVER-134377 and SERVER-135110 for v8.0's Bazel 7.5 and GCC 11.3. Builds execute on
ARM64; tests execute on native s390x hosts.

Use a RHEL ARM64 host matching the target release:

```sh
bazel build install-dist-test --config=linux-s390x-rhel9-cross-gcc-arm64
```

The RHEL8 and RHEL10 configurations use the corresponding release in the name. Docker or Podman must
be available to extract the pinned s390x image filesystem. The cross archive includes GCC, binutils,
libstdc++ and libgcc for the target. The sysroot supplies target glibc, CRT files, OpenSSL and other
system libraries. Only target directories are added to the cross compiler's header and library
search paths. Cross-GCC 11.3 and target runtimes come from the newer archive. Linking preserves
native s390x's GNU gold settings using the archive's ARM64-hosted, multi-target gold 2.37,
which supports s390x. The linker is declared as an action input and selected with `-fuse-ld=gold`
and the archive's bin directory. Setup replaces the `libgcc_s.so` script's `=/usr/lib64/libgcc_s.so.1`
reference with `libgcc_s.so.1` and symlinks that library from the same target sysroot into the GCC
library directory because gold 2.37 does not support the original syntax in script inputs.
Cross builds use GNU-style zlib debug compression: ARM64-hosted v4 gold cannot decompress
s390x ELF `SHF_COMPRESSED` sections, but it supports GNU `.zdebug` sections. Debug compression
remains enabled, and linked output keeps the existing uncompressed debug format.
The native ARM64 toolchain also builds tools such as protoc. Compilation and
linking run on the ARM64 host; remote execution is disabled until RBE workers can consume the new
GCC archives. The target sysroot is extracted separately; native execution tools keep using the host's headers and libraries.

Evergreen applies the cross configuration only on ARM64 hosts. The same variants continue to compile
and execute unit tests and dbtests with native GCC on s390x. The native Python environment is
uploaded separately so downstream s390x tests never consume the ARM64 Python environment. Shared
variants use their own archive and environment paths. A native task records the cross-built mongod
version.

## Validation and publication

The toolchain pins use the published RHEL ARM64 archives from toolchain-builder revision
`1bce2cbf33a4c08af2e73c3b2084e282ca76f3f5` (PRs 155/156), with a SHA-256 for each complete archive.

The smoke targets exercise C++20, libstdc++, 256-byte s390x alignment, and ARM64 build-time
Python with an s390x target platform. The C++ smoke binary links a separate library to check
ARM64 gold's handling of `--start-lib` for s390x objects. The cross configurations and all six
s390x variants select gold for both release and development builds:

```sh
bazel build //bazel/toolchains/cc/mongo_linux_cross:cross_smoke \
  //bazel/toolchains/cc/mongo_linux_cross:cross_linker_smoke \
  //bazel/toolchains/cc/mongo_linux_cross:s390x_target_platform_check \
  --config=opt \
  --config=linux-s390x-rhel9-cross-gcc-arm64
```

Run native jsCore against the produced archive in CI before enabling the new build hosts for
releases. Validate all three releases and the shared variants.
