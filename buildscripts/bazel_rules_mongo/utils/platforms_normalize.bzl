ARCH_NORMALIZE_MAP = {
    "amd64": "x86_64",
    "x86_64": "x86_64",
    "arm64": "aarch64",
    "aarch64": "aarch64",
    "ppc64le": "ppc64le",
    "s390x": "s390x",
}

OS_NORMALIZE_MAP = {
    "macos": "macos",
    "mac os x": "macos",
    "linux": "linux",
    "windows": "windows",
    "windows server 2022": "windows",
}

ARCH_TO_PLATFORM_MAP = {
    "amd64": "@platforms//cpu:x86_64",
    "x86_64": "@platforms//cpu:x86_64",
    "arm64": "@platforms//cpu:arm64",
    "aarch64": "@platforms//cpu:arm64",
    "ppc64le": "@platforms//cpu:ppc",
    "s390x": "@platforms//cpu:s390x",
}

OS_TO_PLATFORM_MAP = {
    "macos": "@platforms//os:osx",
    "linux": "@platforms//os:linux",
    "windows": "@platforms//os:windows",
}
