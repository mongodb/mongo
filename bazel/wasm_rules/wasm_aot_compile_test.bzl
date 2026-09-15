"""Analysis regressions for native and cross-RBE WASM engine selection."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load(":wasm_aot_compile.bzl", "aot_compile_wasm")

EngineSelectionInfo = provider("Selected JavaScript engine for the target configuration.", fields = ["wasm"])

def _engine_selection_impl(ctx):
    return [EngineSelectionInfo(wasm = ctx.attr.wasm)]

_engine_selection = rule(
    implementation = _engine_selection_impl,
    attrs = {"wasm": attr.bool()},
)

def _fake_tool_impl(ctx):
    arch = "s390x" if ctx.target_platform_has_constraint(ctx.attr._s390x[platform_common.ConstraintValueInfo]) else "exec"
    split_link = ctx.attr._split_link[BuildSettingInfo].value
    executable = ctx.actions.declare_file(ctx.label.name + "-" + arch + ("-split-link" if split_link else ""))
    runtime = ctx.actions.declare_file(ctx.label.name + ".runtime")
    ctx.actions.write(executable, "#!/bin/sh\nexit 0\n", is_executable = True)
    ctx.actions.write(runtime, "runtime")
    return [DefaultInfo(executable = executable, runfiles = ctx.runfiles([runtime]))]

_fake_tool = rule(
    implementation = _fake_tool_impl,
    executable = True,
    attrs = {
        "_s390x": attr.label(default = "@platforms//cpu:s390x"),
        "_split_link": attr.label(default = "@rules_rust//rust/settings:experimental_use_cc_common_link"),
    },
)

def _fake_rust_toolchain_impl(_ctx):
    return [platform_common.ToolchainInfo(target_triple = struct(str = "s390x-unknown-linux-gnu"))]

_fake_rust_toolchain = rule(implementation = _fake_rust_toolchain_impl)

def _engine_selection_test_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.equals(env, ctx.attr.expected_wasm, analysistest.target_under_test(env)[EngineSelectionInfo].wasm)
    return analysistest.end(env)

def _aot_action_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = analysistest.target_actions(env)
    asserts.equals(env, 1, len(actions))
    action = actions[0]
    asserts.equals(env, "WasmAotCompile", action.mnemonic)
    asserts.equals(env, "s390x-unknown-linux-gnu", action.argv[action.argv.index("--target") + 1])
    asserts.true(env, any([f.basename == "fake_wasmtime.runtime" for f in action.inputs.to_list()]))
    if ctx.attr.native:
        asserts.true(env, action.argv[0].endswith("fake_wasmtime-s390x-split-link"), action.argv[0])
    else:
        asserts.false(env, "split-link" in action.argv[0], action.argv[0])
    return analysistest.end(env)

_NATIVE = {
    "//command_line_option:platforms": str(Label("//bazel/platforms:rhel9_s390x")),
    str(Label("//bazel/config:js_engine")): "wasm",
}
_CROSS = {
    "//command_line_option:platforms": str(Label("//bazel/platforms:rhel9_s390x_cross")),
    str(Label("//bazel/config:js_engine")): "wasm",
}
_PPC = {
    "//command_line_option:platforms": str(Label("//bazel/platforms:rhel9_ppc64le")),
    str(Label("//bazel/config:js_engine")): "wasm",
}
_FAKE_TOOLCHAIN = {"//command_line_option:extra_toolchains": [str(Label("//bazel/wasm_rules:fake_rust_toolchain"))]}

_native_engine_test = analysistest.make(_engine_selection_test_impl, config_settings = _NATIVE, attrs = {"expected_wasm": attr.bool(default = True)})
_cross_engine_test = analysistest.make(_engine_selection_test_impl, config_settings = _CROSS, attrs = {"expected_wasm": attr.bool(default = True)})
_ppc_engine_test = analysistest.make(_engine_selection_test_impl, config_settings = _PPC, attrs = {"expected_wasm": attr.bool(default = False)})
_native_aot_test = analysistest.make(_aot_action_test_impl, config_settings = _NATIVE | _FAKE_TOOLCHAIN, attrs = {"native": attr.bool(default = False)})
_cross_aot_test = analysistest.make(_aot_action_test_impl, config_settings = _CROSS | _FAKE_TOOLCHAIN, attrs = {"native": attr.bool(default = True)})

def wasm_aot_compile_test_suite(name):
    """Test engine selection and AOT tool configuration without compiling Wasmtime.

    Args:
        name: Name of the test suite.
    """
    _engine_selection(
        name = "engine_selection",
        wasm = select({
            "//bazel/config:js_engine_wasm_supported": True,
            "//bazel/config:js_engine_use_legacy": False,
        }),
        tags = ["manual"],
    )
    _fake_tool(name = "fake_wasmtime", tags = ["manual"])
    _fake_rust_toolchain(name = "fake_rust", tags = ["manual"])
    native.toolchain(
        name = "fake_rust_toolchain",
        toolchain = ":fake_rust",
        toolchain_type = "@rules_rust//rust:toolchain_type",
    )
    aot_compile_wasm(
        name = "aot_fixture",
        tool = ":fake_wasmtime",
        src = "testdata/empty.wasm",
        out = "fixture.cwasm",
        tags = ["manual"],
    )
    tests = []
    for suffix, test_rule, target in [
        ("native_engine", _native_engine_test, ":engine_selection"),
        ("cross_engine", _cross_engine_test, ":engine_selection"),
        ("ppc_engine", _ppc_engine_test, ":engine_selection"),
        ("native_aot", _native_aot_test, ":aot_fixture"),
        ("cross_aot", _cross_aot_test, ":aot_fixture"),
    ]:
        test_rule(name = name + "_" + suffix, target_under_test = target, size = "small")
        tests.append(name + "_" + suffix)
    native.test_suite(name = name, tests = tests)
