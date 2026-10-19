"""Search paths for the v5 IBM GCC cross compiler, never host libraries."""

def cross_lld_bin_dir(arch, repository_name):
    """Locate the ARM64-hosted linker for an IBM cross target."""
    repository_name = "mongo_toolchain_v5" if arch == "ppc64le" else repository_name
    return "external/{}/stow/llvm-v5/bin/".format(repository_name)

def cross_lld_files(arch):
    """Declare linker inputs outside the GCC cross archive."""
    return ["@mongo_toolchain_v5//:lld_files"] if arch == "ppc64le" else []

_ROOT = "external/{toolchain_repo_name}/stow/gcc-v5"
_GCC = _ROOT + "/lib/gcc/{arch}-mongodb-linux/14.2.0"
_TARGET = _ROOT + "/{arch}-mongodb-linux"
_SYSROOT = "external/{sysroot_repo_name}/sysroot"

COMMON_LINK_FLAGS = [_GCC, _TARGET + "/lib64"]
COMMON_BINDIRS = [
    _TARGET + "/bin",
    _GCC,
    _ROOT + "/libexec/gcc/{arch}-mongodb-linux/14.2.0",
    # PPC retains the native toolchain's LLD 19.1.7; s390x keeps its archive's path.
    "{lld_bin_dir}",
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
