# Mold linker
MOLD_VERSION = "2.41.0"

def _mold_entry(arch, sha):
    return {
        "bin_dir": "mold-%s-%s-linux/bin/" % (MOLD_VERSION, arch),
        "sha": sha,
        "url": "https://github.com/rui314/mold/releases/download/v%s/mold-%s-%s-linux.tar.gz" % (MOLD_VERSION, MOLD_VERSION, arch),
    }

MOLD_MAP = {
    "aarch64": _mold_entry("aarch64", "946de2774b06a71346bd59b55fddba610b65b8d93c3a4a1559cc84e103472710"),
    "x86_64": _mold_entry("x86_64", "a3696680d99e692970590a178bc3a33d78d60d1c6dc9db7a11b557b02b751f5d"),
    "s390x": _mold_entry("s390x", "e5e42a2685967ad209d7568e29a0b2ea86ec9d462cbf5d4aa7398a8087ee1a9d"),
    "ppc64le": _mold_entry("ppc64le", "b1a045d9dcbdb1af523518beca9da38f3ee0f208964851831a0664cafaf9bc70"),
}
