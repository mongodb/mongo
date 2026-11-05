"""Check the resolved directory passed to tar and zip for each release platform."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@rules_pkg//:pkg.bzl", "pkg_tar", "pkg_zip")
load("@rules_pkg//pkg:providers.bzl", "PackageVariablesInfo")
load(":archive_name.bzl", "mongo_archive_name")

def _archive_name_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic in ["PackageTar", "PackageZip"]
    ]
    asserts.equals(env, 1, len(actions))
    argv = actions[0].argv
    flag = "--directory" if actions[0].mnemonic == "PackageTar" else "-d"
    directory = argv[argv.index(flag) + 1] if flag in argv else [
        arg[len(flag) + 1:]
        for arg in argv
        if arg.startswith(flag + "=")
    ][0]
    asserts.equals(env, ctx.attr.expected_directory, directory)
    return analysistest.end(env)

_PLATFORMS = [
    ("linux", "x86_64", "x86_64", "linux", "rhel9"),
    ("linux", "arm64", "aarch64", "linux", "rhel9"),
    ("linux", "ppc64le", "ppc64le", "linux", "rhel9"),
    ("linux", "s390x", "s390x", "linux", "rhel9"),
    ("linux", "riscv64", "riscv64", "linux", "rhel9"),
    ("macos", "x86_64", "x86_64", "macos", ""),
    ("macos", "arm64", "aarch64", "macos", ""),
    ("windows", "x86_64", "x86_64", "win32", "windows"),
]

def _test_rule(os, cpu, enterprise):
    return analysistest.make(
        _archive_name_test_impl,
        attrs = {"expected_directory": attr.string(mandatory = True)},
        config_settings = {
            "//command_line_option:platforms": str(Label("//bazel/install_rules:archive_name_platform_" + os + "_" + cpu)),
            str(Label("//bazel/config:build_enterprise")): enterprise,
        },
    )

_linux_x86_64_enterprise_test = _test_rule("linux", "x86_64", True)
_linux_x86_64_community_test = _test_rule("linux", "x86_64", False)
_linux_arm64_enterprise_test = _test_rule("linux", "arm64", True)
_linux_arm64_community_test = _test_rule("linux", "arm64", False)
_linux_ppc64le_enterprise_test = _test_rule("linux", "ppc64le", True)
_linux_ppc64le_community_test = _test_rule("linux", "ppc64le", False)
_linux_s390x_enterprise_test = _test_rule("linux", "s390x", True)
_linux_s390x_community_test = _test_rule("linux", "s390x", False)
_macos_x86_64_enterprise_test = _test_rule("macos", "x86_64", True)
_macos_x86_64_community_test = _test_rule("macos", "x86_64", False)
_macos_arm64_enterprise_test = _test_rule("macos", "arm64", True)
_macos_arm64_community_test = _test_rule("macos", "arm64", False)
_windows_x86_64_enterprise_test = _test_rule("windows", "x86_64", True)
_windows_x86_64_community_test = _test_rule("windows", "x86_64", False)

_TEST_RULES = {
    ("linux", "x86_64"): [_linux_x86_64_enterprise_test, _linux_x86_64_community_test],
    ("linux", "arm64"): [_linux_arm64_enterprise_test, _linux_arm64_community_test],
    ("linux", "ppc64le"): [_linux_ppc64le_enterprise_test, _linux_ppc64le_community_test],
    ("linux", "s390x"): [_linux_s390x_enterprise_test, _linux_s390x_community_test],
    ("macos", "x86_64"): [_macos_x86_64_enterprise_test, _macos_x86_64_community_test],
    ("macos", "arm64"): [_macos_arm64_enterprise_test, _macos_arm64_community_test],
    ("windows", "x86_64"): [_windows_x86_64_enterprise_test, _windows_x86_64_community_test],
}

def _fixture_variables_impl(ctx):
    return [PackageVariablesInfo(values = {
        "MONGO_DISTMOD": ctx.attr.distmod,
        "MONGO_VERSION": "9.0.7",
    })]

_fixture_variables = rule(
    implementation = _fixture_variables_impl,
    attrs = {"distmod": attr.string()},
)

def archive_name_test_suite(name):
    for os, cpu, arch, archive_os, distmod in _PLATFORMS:
        native.platform(
            name = "archive_name_platform_" + os + "_" + cpu,
            constraint_values = ["@platforms//os:" + os, "@platforms//cpu:" + ("ppc" if cpu == "ppc64le" else cpu)],
            tags = ["manual"],
        )

    for os, cpu, arch, archive_os, distmod in _PLATFORMS:
        fixture_prefix = name + "_" + os + "_" + cpu
        _fixture_variables(
            name = fixture_prefix + "_variables",
            distmod = distmod,
        )
        for format, archive_rule in [("tar", pkg_tar), ("zip", pkg_zip)]:
            archive_rule(
                name = fixture_prefix + "_" + format + "_fixture",
                srcs = ["archive_name.bzl"],
                package_dir = mongo_archive_name(),
                package_variables = fixture_prefix + "_variables",
                tags = ["manual"],
                testonly = True,
            )

    tests = []
    for os, cpu, arch, archive_os, distmod in _PLATFORMS:
        # select() configuration errors occur before analysistest can capture
        # them. Keep the unsupported CPU fixtures for direct failure checks.
        if cpu == "riscv64":
            continue
        for enterprise in [True, False]:
            expected = "mongodb-" + archive_os + "-" + arch
            if enterprise:
                expected += "-enterprise"
            expected += "-" + distmod + "-9.0.7"
            for format in ["tar", "zip"]:
                test_name = name + "_" + os + "_" + cpu + "_" + ("enterprise" if enterprise else "community") + "_" + format
                _TEST_RULES[(os, cpu)][0 if enterprise else 1](
                    name = test_name,
                    size = "small",
                    target_under_test = name + "_" + os + "_" + cpu + "_" + format + "_fixture",
                    expected_directory = expected,
                )
                tests.append(test_name)
    native.test_suite(name = name, tests = tests)
