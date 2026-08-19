COMMON_LINK_FLAGS = [
    "{toolchain_repo_dir}/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0",
    "{toolchain_repo_dir}/v5/lib",
    "{toolchain_repo_dir}/v5/lib64",
]

COMMON_BUILTIN_INCLUDE_DIRECTORIES = [
    "/usr/include/openssl",
    "/usr/include/{arch}-mongodb-linux",
    "/usr/include",
]

COMMON_INCLUDE_DIRECTORIES = [
    "{toolchain_repo_dir}/stow/gcc-v5/include/c++/14.2.0",
    "{toolchain_repo_dir}/stow/gcc-v5/include/c++/14.2.0/{arch}-mongodb-linux",
]

COMMON_BINDIRS = [
    "{toolchain_repo_dir}/v5/bin",
    "{toolchain_repo_dir}/stow/gcc-v5/libexec/gcc/{arch}-mongodb-linux/14.2.0",
    "{toolchain_repo_dir}/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0",
    "{toolchain_repo_dir}/stow/llvm-v5/bin",
]

GCC_INCLUDE_DIRS = [
    "{toolchain_repo_dir}/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0/include",
    "{toolchain_repo_dir}/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0/include-fixed",
]

CLANG_INCLUDE_DIRS = [
    "{toolchain_repo_dir}/stow/gcc-v5/include/c++/14.2.0/backward",
    "{toolchain_repo_dir}/stow/llvm-v5/lib/clang/19/include",
]

def clang_resource_dir(toolchain_repo_dir):
    return toolchain_repo_dir + "/stow/llvm-v5/lib/clang/19/"
