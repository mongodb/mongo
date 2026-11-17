"""Verify target and build-tool Python runtimes in a Windows cross configuration."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@py_windows_x86_64//:availability.bzl", WINDOWS_PYTHON_AVAILABLE = "PYTHON_RUNTIME_AVAILABLE")
load("@py_linux_x86_64//:availability.bzl", LINUX_PYTHON_AVAILABLE = "PYTHON_RUNTIME_AVAILABLE")
load("@rules_python//python:defs.bzl", "py_runtime", "py_runtime_pair")
load(":runtime_toolchains.bzl", "python_runtime_toolchains")

_RuntimeInfo = provider(fields = ["interpreter", "tool_interpreter"])

def _probe_impl(ctx):
    runtime = ctx.toolchains["@rules_python//python:toolchain_type"].py3_runtime
    return [_RuntimeInfo(
        interpreter = runtime.interpreter.path,
        tool_interpreter = ctx.attr.tool[_RuntimeInfo].interpreter if ctx.attr.tool else None,
    )]

_probe = rule(
    implementation = _probe_impl,
    attrs = {"tool": attr.label(cfg = "exec")},
    toolchains = ["@rules_python//python:toolchain_type"],
)

def _test_impl(ctx):
    env = analysistest.begin(ctx)
    runtime = analysistest.target_under_test(env)[_RuntimeInfo]
    asserts.true(env, runtime.interpreter.endswith("/dist/python.exe"), runtime.interpreter)
    asserts.true(env, runtime.tool_interpreter.endswith("/dist/bin/python3"), runtime.tool_interpreter)
    return analysistest.end(env)

_test = analysistest.make(
    _test_impl,
    config_settings = {
        "//command_line_option:platforms": "@@//bazel/platforms:windows_x86_64_cross",
        "//command_line_option:extra_execution_platforms": ["@@//bazel/platforms:ubuntu24_amd64_windows_cross_exec"],
    },
)

def windows_cross_python_runtime_test(name):
    _probe(name = name + "_tool", tags = ["manual"])
    _probe(name = name + "_probe", tool = ":" + name + "_tool", tags = ["manual"])
    _test(
        name = name,
        target_under_test = ":" + name + "_probe",
        target_compatible_with = [] if WINDOWS_PYTHON_AVAILABLE and LINUX_PYTHON_AVAILABLE else ["@platforms//:incompatible"],
    )

def _execution_runtime_test_impl(ctx):
    env = analysistest.begin(ctx)
    runtime = analysistest.target_under_test(env)[_RuntimeInfo]
    asserts.true(env, runtime.interpreter.endswith(ctx.attr.expected_runtime), runtime.interpreter)
    asserts.true(env, runtime.tool_interpreter.endswith(ctx.attr.expected_tool_runtime), runtime.tool_interpreter)
    return analysistest.end(env)

def _runtime_test(target_platform, execution_platform, host_platform, runtime, tool_runtime):
    return analysistest.make(
        _execution_runtime_test_impl,
        attrs = {
            "expected_runtime": attr.string(default = runtime),
            "expected_tool_runtime": attr.string(default = tool_runtime),
        },
        config_settings = {
            "//command_line_option:platforms": "@@//bazel/platforms:" + target_platform,
            "//command_line_option:extra_execution_platforms": ["@@//bazel/toolchains/python:" + execution_platform],
            "//command_line_option:host_platform": "@@//bazel/toolchains/python:" + host_platform,
            "//command_line_option:extra_toolchains": [
                # Extra toolchains give the last entry highest priority.
                "@@//bazel/toolchains/python:fixture_linux_toolchain",
                "@@//bazel/toolchains/python:fixture_windows_toolchain",
                "@@//bazel/toolchains/python:fixture_windows_toolchain_exec",
            ],
        },
    )

_native_wasi_test = _runtime_test("wasm32", "fixture_windows_execution", "fixture_windows_execution", "/python.exe", "/python.exe")
_cross_windows_test = _runtime_test("windows_x86_64_cross", "fixture_linux_execution", "fixture_windows_execution", "/python.exe", "/python3")
_linux_wasi_test = _runtime_test("wasm32", "fixture_linux_execution", "fixture_linux_execution", "/python3", "/python3")

def python_execution_runtime_test_suite(name):
    for os, interpreter in [("windows", "python.exe"), ("linux", "python3")]:
        constraints = ["@platforms//os:" + os, "@platforms//cpu:x86_64"]
        native.platform(name = "fixture_" + os + "_execution", constraint_values = constraints)
        py_runtime(
            name = "fixture_" + os + "_runtime",
            interpreter = "testdata/" + interpreter,
            python_version = "PY3",
            tags = ["manual"],
        )
        py_runtime_pair(
            name = "fixture_" + os + "_runtime_pair",
            py3_runtime = ":fixture_" + os + "_runtime",
            tags = ["manual"],
        )
        python_runtime_toolchains(
            name = "fixture_" + os + "_toolchain",
            runtime_pair = ":fixture_" + os + "_runtime_pair",
            constraints = constraints,
            windows = os == "windows",
        )
    _probe(name = name + "_tool", tags = ["manual"])
    _probe(name = name + "_probe", tool = ":" + name + "_tool", tags = ["manual"])
    tests = []
    for suffix, test in [("native_wasi", _native_wasi_test), ("cross_windows", _cross_windows_test), ("linux_wasi", _linux_wasi_test)]:
        test(name = name + "_" + suffix, target_under_test = ":" + name + "_probe")
        tests.append(":" + name + "_" + suffix)
    native.test_suite(name = name, tests = tests)
