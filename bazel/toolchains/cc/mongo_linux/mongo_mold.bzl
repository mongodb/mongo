# Mold linker
MOLD_VERSION = "2.40.4"

def _mold_entry(arch, sha):
    return {
        "bin_dir": "mold-%s-%s-linux/bin/" % (MOLD_VERSION, arch),
        "sha": sha,
        "url": "https://github.com/rui314/mold/releases/download/v%s/mold-%s-%s-linux.tar.gz" % (MOLD_VERSION, MOLD_VERSION, arch),
    }

MOLD_MAP = {
    "aarch64": _mold_entry("aarch64", "c799b9ccae8728793da2186718fbe53b76400a9da396184fac0c64aa3298ec37"),
    "x86_64": _mold_entry("x86_64", "4c999e19ffa31afa5aa429c679b665d5e2ca5a6b6832ad4b79668e8dcf3d8ec1"),
    "s390x": _mold_entry("s390x", "79cc0a7e596dfbb8b05835f91222c24468278438369ec4a7afa70abb4a84158b"),
    "ppc64le": _mold_entry("ppc64le", "81e6a2531d4e6b3a62163de04d63fc5f845a5f00ad13fde8b89856206c93a9f9"),
}
