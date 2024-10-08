ARCH_NORMALIZE_MAP = {
    "amd64": "x86_64",
    "x86_64": "x86_64",
    "arm64": "aarch64",
    "aarch64": "aarch64",
    "ppc64le": "ppc64le",
    "s390x": "s390x",
}

COMMON_LINK_FLAGS = [
    "external/mongo_toolchain/stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0",
    "external/mongo_toolchain/v4/lib",
    "external/mongo_toolchain/v4/lib64",
]

COMMON_BUILTIN_INCLUDE_DIRECTORIES = [
    "/usr/include/openssl",
    "/usr/include/{arch}-linux-gnu",
    "/usr/include",
]

COMMON_INCLUDE_DIRECTORIES = [
    "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0",
    "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0/{arch}-mongodb-linux",
]

COMMON_BINDIRS = [
    "external/mongo_toolchain/v4/bin",
    "external/mongo_toolchain/stow/gcc-v4/libexec/gcc/{arch}-mongodb-linux/11.3.0",
    "external/mongo_toolchain/stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0",
    "external/mongo_toolchain/stow/llvm-v4/bin",
]

GCC_INCLUDE_DIRS = [
    "external/mongo_toolchain/stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0/include",
    "external/mongo_toolchain/stow/gcc-v4/lib/gcc/{arch}-mongodb-linux/11.3.0/include-fixed",
]

CLANG_INCLUDE_DIRS = [
    "external/mongo_toolchain/stow/gcc-v4/include/c++/11.3.0/backward",
    "external/mongo_toolchain/stow/llvm-v4/lib/clang/12.0.1/include",
]
