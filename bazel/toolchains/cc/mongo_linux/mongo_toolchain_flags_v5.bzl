COMMON_LINK_FLAGS = [
    "external/{toolchain_repo_name}/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0",
    "external/{toolchain_repo_name}/v5/lib",
    "external/{toolchain_repo_name}/v5/lib64",
]

COMMON_BUILTIN_INCLUDE_DIRECTORIES = [
    "/usr/include/openssl",
    "/usr/include/{arch}-mongodb-linux",
    "/usr/include",
]

COMMON_INCLUDE_DIRECTORIES = [
    "external/{toolchain_repo_name}/stow/gcc-v5/include/c++/14.2.0",
    "external/{toolchain_repo_name}/stow/gcc-v5/include/c++/14.2.0/{arch}-mongodb-linux",
]

COMMON_BINDIRS = [
    "external/{toolchain_repo_name}/v5/bin",
    "external/{toolchain_repo_name}/stow/gcc-v5/libexec/gcc/{arch}-mongodb-linux/14.2.0",
    "external/{toolchain_repo_name}/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0",
    "external/{toolchain_repo_name}/stow/llvm-v5/bin",
]

GCC_INCLUDE_DIRS = [
    "external/{toolchain_repo_name}/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0/include",
    "external/{toolchain_repo_name}/stow/gcc-v5/lib/gcc/{arch}-mongodb-linux/14.2.0/include-fixed",
]

CLANG_INCLUDE_DIRS = [
    "external/{toolchain_repo_name}/stow/gcc-v5/include/c++/14.2.0/backward",
    "external/{toolchain_repo_name}/stow/llvm-v5/lib/clang/19/include",
]

def clang_resource_dir(toolchain_repo_name):
    return "external/{}/stow/llvm-v5/lib/clang/19/".format(toolchain_repo_name)
