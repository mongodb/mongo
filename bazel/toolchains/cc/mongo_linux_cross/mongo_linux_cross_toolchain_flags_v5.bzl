"""Search paths for the composed v5 Linux PPC64LE/s390x cross toolchain."""

load(":openssl_overrides.bzl", "OPENSSL_INCLUDE_DIRS", "OPENSSL_LINK_DIRS")

# Compiler and LLVM tools come from the execution-architecture archive at the
# repository root. Target C++ headers, CRT objects, and runtime libraries come
# from the target-architecture archive extracted under target/.
COMMON_LINK_FLAGS = OPENSSL_LINK_DIRS + [
    "{toolchain_repo_dir}/target/stow/gcc-v5/lib64",
    "{toolchain_repo_dir}/target/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0",
    "{toolchain_repo_dir}/target/v5/lib",
    "{toolchain_repo_dir}/target/v5/lib64",
]

COMMON_BUILTIN_INCLUDE_DIRECTORIES = OPENSSL_INCLUDE_DIRS + [
    "/usr/include/openssl",
    "/usr/include/{arch}-mongodb-linux",
    "/usr/include",
]

COMMON_INCLUDE_DIRECTORIES = [
    "{toolchain_repo_dir}/target/stow/gcc-v5/include/c++/14.2.0",
    "{toolchain_repo_dir}/target/stow/gcc-v5/include/c++/14.2.0/{arch}-mongodb-linux",
]

COMMON_BINDIRS = [
    "{toolchain_repo_dir}/target/stow/gcc-v5/libexec/gcc/{arch}-mongodb-linux/14.2.0",
    "{toolchain_repo_dir}/target/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0",
    "{toolchain_repo_dir}/v5/bin",
    "{toolchain_repo_dir}/stow/llvm-v5/bin",
]

GCC_INCLUDE_DIRS = [
    "{toolchain_repo_dir}/target/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0/include",
    "{toolchain_repo_dir}/target/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0/include-fixed",
]

CLANG_INCLUDE_DIRS = [
    "{toolchain_repo_dir}/target/stow/gcc-v5/include/c++/14.2.0/backward",
    "{toolchain_repo_dir}/target/stow/llvm-v5/lib/clang/19/include",
]

def clang_resource_dir(toolchain_repo_dir):
    return toolchain_repo_dir + "/target/stow/llvm-v5/lib/clang/19/"
