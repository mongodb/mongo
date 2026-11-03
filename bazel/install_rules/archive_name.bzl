"""Distribution archive directory naming shared by packaging and its tests."""

def mongo_archive_name():
    # The archive's execution host may have a different architecture from the
    # packaged binaries. Resolve both OS and CPU from the target platform.
    return select({
        "@platforms//os:linux": "mongodb-linux-",
        "@platforms//os:windows": "mongodb-win32-",
        "@platforms//os:macos": "mongodb-macos-",
        "//conditions:default": "mongodb-",
    }) + select({
        "@platforms//cpu:x86_64": "x86_64",
        "@platforms//cpu:arm64": "aarch64",
        "@platforms//cpu:ppc64le": "ppc64le",
        "@platforms//cpu:s390x": "s390x",
    }, no_match_error = "Unsupported target CPU for MongoDB distribution archives. " +
                        "Supported target CPUs: x86_64, arm64 (aarch64), ppc64le, s390x. " +
                        "Select a supported target platform with --platforms or add an explicit target CPU mapping in bazel/install_rules/archive_name.bzl.") + select({
        "//bazel/config:build_enterprise_linux_enabled": "-enterprise",
        "//bazel/config:build_enterprise_windows_enabled": "-enterprise",
        "//bazel/config:build_enterprise_mac_enabled": "-enterprise",
        "//conditions:default": "",
    }) + "-{MONGO_DISTMOD}-{MONGO_VERSION}"
