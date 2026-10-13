"""Search paths for the v5 IBM GCC cross compiler, never host libraries."""

_ROOT = "external/{toolchain_repo_name}/stow/gcc-v5"
_GCC = _ROOT + "/lib/gcc/{arch}-mongodb-linux/14.2.0"
_TARGET = _ROOT + "/{arch}-mongodb-linux"
_SYSROOT = "external/{sysroot_repo_name}/sysroot"

COMMON_LINK_FLAGS = [_GCC, _TARGET + "/lib64"]
COMMON_BINDIRS = [
    _TARGET + "/bin",
    _GCC,
    _ROOT + "/libexec/gcc/{arch}-mongodb-linux/14.2.0",
    # Optimized PPC GCC builds select LLD. This is the ARM64 linker bundled
    # with the cross archive; do not rely on a linker installed on the host.
    "external/{toolchain_repo_name}/stow/llvm-v5/bin",
]
COMMON_INCLUDE_DIRECTORIES = [
    _TARGET + "/include/c++/14.2.0",
    _TARGET + "/include/c++/14.2.0/{arch}-mongodb-linux",
    _TARGET + "/include/c++/14.2.0/backward",
]
GCC_INCLUDE_DIRS = [_GCC + "/include", _GCC + "/include-fixed"]
COMMON_BUILTIN_INCLUDE_DIRECTORIES = [
    _SYSROOT + "/usr/include/openssl",
    _SYSROOT + "/usr/include",
]
CLANG_INCLUDE_DIRS = []
