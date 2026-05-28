"""Thin Mongo wrappers around upstream vendored tcmalloc tests."""

load("@com_google_tcmalloc//tcmalloc:variants.bzl", "test_variants")
load("//bazel:mongo_src_rules.bzl", "PDB_GENERATION_ENABLED", "SEPARATE_DEBUG_ENABLED")
load("//bazel:separate_debug.bzl", "extract_debuginfo_test")
load(
    "//src/third_party/tcmalloc:upstream_tcmalloc_test_manifest.bzl",
    "TCMALLOC_DIRECT_TESTS",
    "TCMALLOC_VARIANT_TEST_SUITES",
)

_TCMALLOC_PLATFORM_SUPPORTED = select({
    "//bazel/config:linux_s390x": ["@platforms//:incompatible"],
    "//bazel/config:linux_ppc64le": ["@platforms//:incompatible"],
    "@platforms//os:macos": ["@platforms//:incompatible"],
    "@platforms//os:windows": ["@platforms//:incompatible"],
    "//conditions:default": [],
})

_PACKAGES = [
    "tcmalloc",
    "tcmalloc/internal",
    "tcmalloc/testing",
    "tcmalloc/selsan",
]
_SHADOW_PREFIXES = {
    "tcmalloc": "upstream_tcmalloc_",
    "tcmalloc/internal": "upstream_tcmalloc_internal_",
    "tcmalloc/testing": "upstream_tcmalloc_testing_",
    "tcmalloc/selsan": "upstream_tcmalloc_selsan_",
}

_KNOWN_FAILING_DIRECT_TESTS = {
    "tcmalloc": {
        "guarded_page_allocator_profile_test": True,
    },
    "tcmalloc/internal": {
        "allocation_guard_test": True,
    },
}

_KNOWN_FAILING_VARIANT_SUITES = {
    "tcmalloc/testing": {
        "memory_errors_test": True,
        "profile_drop_frames_test": True,
    },
}

_KNOWN_FAILING_VARIANT_TESTS = {
    "tcmalloc/testing": {
        "releasing_test_small_but_slow": True,
        "releasing_test_small_but_slow_lowfrag_sizeclasses": True,
    },
}

def _exported_test_label(package_name, target_name):
    return "@com_google_tcmalloc//" + package_name + ":mongo_export_" + target_name

def _is_known_failure(package_name, upstream_name):
    if package_name in _KNOWN_FAILING_DIRECT_TESTS:
        if upstream_name in _KNOWN_FAILING_DIRECT_TESTS[package_name]:
            return True
    if package_name in _KNOWN_FAILING_VARIANT_TESTS:
        if upstream_name in _KNOWN_FAILING_VARIANT_TESTS[package_name]:
            return True
    if package_name in _KNOWN_FAILING_VARIANT_SUITES:
        for suite_name in _KNOWN_FAILING_VARIANT_SUITES[package_name]:
            if upstream_name.startswith(suite_name + "_"):
                return True
    return False

def _wrapper_tags(package_name, upstream_name):
    if _is_known_failure(package_name, upstream_name):
        return [
            "manual",
            "mongo_tcmalloc_known_failure",
        ]

    tags = [
        "mongo_tcmalloc_unittest",
    ]
    return tags

def _declare_known_failure_marker(shadow_name, tags):
    native.sh_test(
        name = "known_failure_skipped_" + shadow_name,
        srcs = ["//src/third_party/tcmalloc:known_failure_skipped_test.sh"],
        args = ["//" + native.package_name() + ":" + shadow_name],
        tags = [
            "manual",
            "mongo_tcmalloc_known_failure_marker",
            "mongo_tcmalloc_unittest",
        ] + tags,
        target_compatible_with = _target_compatible_with(tags),
    )

def _target_compatible_with(tags):
    compat = [] + _TCMALLOC_PLATFORM_SUPPORTED
    if "nosan" in tags:
        compat += select({
            "//bazel/config:any_enabled_sanitizer": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })
        return compat
    if "noasan" in tags:
        compat += select({
            "//bazel/config:asan_enabled": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })
    if "nomsan" in tags:
        compat += select({
            "//bazel/config:msan_enabled": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })
    if "notsan" in tags:
        compat += select({
            "//bazel/config:tsan_enabled": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })
    if "noubsan" in tags:
        compat += select({
            "//bazel/config:ubsan_enabled": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })
    return compat

def _declare_test_wrapper(package_name, shadow_name, upstream_name, tags):
    actual = _exported_test_label(package_name, upstream_name)
    extract_debuginfo_test(
        name = shadow_name,
        binary_with_debug = actual,
        deps = [actual],
        type = "program",
        enabled = SEPARATE_DEBUG_ENABLED,
        enable_pdb = PDB_GENERATION_ENABLED,
        tags = _wrapper_tags(package_name, upstream_name),
        target_compatible_with = _target_compatible_with(tags),
    )
    if _is_known_failure(package_name, upstream_name):
        _declare_known_failure_marker(shadow_name, tags)

def _declare_variant_suite(package_name, shadow_prefix, suite_name, tags):
    variant_targets = []
    for variant in test_variants:
        upstream_name = suite_name + "_" + variant["name"]
        shadow_name = shadow_prefix + upstream_name
        _declare_test_wrapper(package_name, shadow_name, upstream_name, tags)
        if not _is_known_failure(package_name, upstream_name):
            variant_targets.append(shadow_name)
    native.test_suite(
        name = shadow_prefix + suite_name,
        tests = variant_targets,
    )

def define_upstream_tcmalloc_test_targets(name = None):
    """Declares Mongo wrappers around vendored upstream tcmalloc tests.

    Args:
      name: Unused compatibility parameter required by buildifier for public macros.
    """
    if name != None:
        fail("define_upstream_tcmalloc_test_targets() does not take a name")

    for package_name in _PACKAGES:
        shadow_prefix = _SHADOW_PREFIXES[package_name]
        for target_name, tags in TCMALLOC_DIRECT_TESTS[package_name]:
            _declare_test_wrapper(package_name, shadow_prefix + target_name, target_name, tags)
        for suite_name, tags in TCMALLOC_VARIANT_TEST_SUITES[package_name]:
            _declare_variant_suite(package_name, shadow_prefix, suite_name, tags)
