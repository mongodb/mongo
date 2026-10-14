"""Search paths for the v4 s390x GCC cross compiler, never host libraries."""

_ROOT = "external/{toolchain_repo_name}/stow/gcc-v4"
_GCC = _ROOT + "/lib/gcc/s390x-mongodb-linux/11.3.0"
_TARGET = _ROOT + "/s390x-mongodb-linux"
_SYSROOT = "external/s390x_cross_sysroot/sysroot"

COMMON_LINK_FLAGS = [_GCC, _TARGET + "/lib64"]
COMMON_BINDIRS = [
    _TARGET + "/bin",
    _GCC,
    _ROOT + "/libexec/gcc/s390x-mongodb-linux/11.3.0",
]
COMMON_INCLUDE_DIRECTORIES = [
    _TARGET + "/include/c++/11.3.0",
    _TARGET + "/include/c++/11.3.0/s390x-mongodb-linux",
    _TARGET + "/include/c++/11.3.0/backward",
]
GCC_INCLUDE_DIRS = [_GCC + "/include", _GCC + "/include-fixed"]
COMMON_BUILTIN_INCLUDE_DIRECTORIES = [
    _SYSROOT + "/usr/include/openssl",
    _SYSROOT + "/usr/include",
]
CLANG_INCLUDE_DIRS = []
