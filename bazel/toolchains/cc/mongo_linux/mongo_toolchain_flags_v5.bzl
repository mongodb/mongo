COMMON_LINK_FLAGS = [
    "external/mongo_toolchain_v5/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0",
    "external/mongo_toolchain_v5/v5/lib",
    "external/mongo_toolchain_v5/v5/lib64",
]

COMMON_BUILTIN_INCLUDE_DIRECTORIES = [
    "/usr/include/openssl",
    "/usr/include/{arch}-mongodb-linux",
    "/usr/include",
]

COMMON_INCLUDE_DIRECTORIES = [
    "external/mongo_toolchain_v5/stow/gcc-v5/include/c++/14.2.0",
    "external/mongo_toolchain_v5/stow/gcc-v5/include/c++/14.2.0/{arch}-mongodb-linux",
]

COMMON_BINDIRS = [
    "external/mongo_toolchain_v5/v5/bin",
    "external/mongo_toolchain_v5/stow/gcc-v5/libexec/gcc/{arch}-mongodb-linux/14.2.0",
    "external/mongo_toolchain_v5/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0",
    "external/mongo_toolchain_v5/stow/llvm-v5/bin",
]

GCC_INCLUDE_DIRS = [
    "external/mongo_toolchain_v5/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0/include",
    "external/mongo_toolchain_v5/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0/include-fixed",
]

CLANG_INCLUDE_DIRS = [
    "external/mongo_toolchain_v5/stow/gcc-v5/include/c++/14.2.0/backward",
    "external/mongo_toolchain_v5/stow/llvm-v5/lib/clang/19/include",
]

CLANG_RESOURCE_DIR = "external/mongo_toolchain_v5/stow/llvm-v5/lib/clang/19/"
