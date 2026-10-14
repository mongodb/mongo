"""Verify architecture metadata against actual target platform configurations."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load(":target_platform_arch.bzl", "TARGET_ARCH_CONSTRAINTS", "target_platform_arch")

_Architecture = provider(fields = ["value"])

def _architecture_probe_impl(ctx):
    return [_Architecture(value = target_platform_arch(ctx))]

_architecture_probe = rule(
    implementation = _architecture_probe_impl,
    attrs = TARGET_ARCH_CONSTRAINTS,
)

def _architecture_test_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.equals(env, ctx.attr.expected, analysistest.target_under_test(env)[_Architecture].value)
    return analysistest.end(env)

_CASES = [
    ("linux_x86_64", "linux", "x86_64", "x86_64"),
    ("linux_arm64", "linux", "arm64", "aarch64"),
    ("linux_aarch64", "linux", "aarch64", "aarch64"),
    ("linux_ppc", "linux", "ppc", "ppc64le"),
    ("linux_ppc64le", "linux", "ppc64le", "ppc64le"),
    ("linux_s390x", "linux", "s390x", "s390x"),
    ("macos_x86_64", "macos", "x86_64", "x86_64"),
    ("macos_arm64", "macos", "arm64", "arm64"),
    ("windows_x86_64", "windows", "x86_64", "amd64"),
    ("windows_arm64", "windows", "arm64", "arm64"),
    ("wasi_wasm32", "wasi", "wasm32", "wasm32"),
]

linux_x86_64_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_linux_x86_64"))},
)

linux_arm64_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_linux_arm64"))},
)

linux_aarch64_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_linux_aarch64"))},
)

linux_ppc_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_linux_ppc"))},
)

linux_ppc64le_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_linux_ppc64le"))},
)

linux_s390x_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_linux_s390x"))},
)

macos_x86_64_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_macos_x86_64"))},
)

macos_arm64_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_macos_arm64"))},
)

windows_x86_64_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_windows_x86_64"))},
)

windows_arm64_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_windows_arm64"))},
)

wasi_wasm32_test = analysistest.make(
    _architecture_test_impl,
    attrs = {"expected": attr.string()},
    config_settings = {"//command_line_option:platforms": str(Label("//bazel/config:buildinfo_wasi_wasm32"))},
)

_architecture_tests = {
    "linux_x86_64": linux_x86_64_test,
    "linux_arm64": linux_arm64_test,
    "linux_aarch64": linux_aarch64_test,
    "linux_ppc": linux_ppc_test,
    "linux_ppc64le": linux_ppc64le_test,
    "linux_s390x": linux_s390x_test,
    "macos_x86_64": macos_x86_64_test,
    "macos_arm64": macos_arm64_test,
    "windows_x86_64": windows_x86_64_test,
    "windows_arm64": windows_arm64_test,
    "wasi_wasm32": wasi_wasm32_test,
}

def target_platform_arch_test_suite(name):
    _architecture_probe(name = "buildinfo_architecture_probe")
    tests = []
    for case, os, cpu, expected in _CASES:
        native.platform(
            name = "buildinfo_" + case,
            constraint_values = ["@platforms//os:" + os, "@platforms//cpu:" + cpu],
        )
        test_name = "buildinfo_" + case + "_test"
        _architecture_tests[case](
            name = test_name,
            target_under_test = ":buildinfo_architecture_probe",
            expected = expected,
        )
        tests.append(":" + test_name)
    native.test_suite(name = name, tests = tests)
