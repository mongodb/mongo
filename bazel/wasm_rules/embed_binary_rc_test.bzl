"""Analysis regressions for native and cross Windows binary resource embedding."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load(":embed_binary.bzl", "embed_binary_rc")

def _embedding_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = analysistest.target_actions(env)
    rc_actions = [action for action in actions if action.mnemonic == "WindowsRC"]
    asserts.equals(env, 1, len(rc_actions))
    if not rc_actions:
        return analysistest.end(env)
    action = rc_actions[0]
    target = analysistest.target_under_test(env)
    output = target[DefaultInfo].files.to_list()[0]
    asserts.equals(env, "fixture.res", output.basename)
    linker = target[CcInfo].linking_context.linker_inputs.to_list()[0]
    asserts.equals(env, [output.path], linker.user_link_flags)
    asserts.equals(env, [output], list(linker.additional_inputs))
    asserts.true(env, any([file.basename == "empty.wasm" for file in action.inputs.to_list()]))
    generated = [action for action in actions if action.mnemonic == "FileWrite"][0]
    asserts.true(env, "testdata/empty.wasm" in generated.content)
    asserts.false(env, "\\" in generated.content, "RC paths must work on the Linux worker")
    if ctx.attr.cross:
        asserts.true(env, any(["windows_rc_action_test_clang" in file.path for file in action.inputs.to_list()]))
        asserts.true(env, any([file.basename == "winres.h" for file in action.inputs.to_list()]))
        asserts.true(env, any(["sysroot/winsdk/include/um" in arg for arg in action.argv]))
        asserts.true(env, "runtime_libs" in action.env.get("LD_LIBRARY_PATH", ""))
        asserts.true(env, "bazel-out" in action.env.get("PATH", ""))
        for key in ["TEMP", "TMP", "TMPDIR"]:
            asserts.equals(env, "/tmp", action.env.get(key))
    else:
        for key in ["PATH", "LD_LIBRARY_PATH", "TEMP", "TMP", "TMPDIR"]:
            asserts.false(env, key in action.env)
        asserts.false(env, any([file.basename == "winres.h" for file in action.inputs.to_list()]))
    return analysistest.end(env)

_native_test = analysistest.make(
    _embedding_test_impl,
    attrs = {"cross": attr.bool(default = False)},
    config_settings = {"//command_line_option:platforms": "@@//bazel/platforms:windows_amd64"},
)
_cross_test = analysistest.make(
    _embedding_test_impl,
    attrs = {"cross": attr.bool(default = True)},
    config_settings = {"//command_line_option:platforms": "@@//bazel/platforms:windows_x86_64_cross"},
)

def embed_binary_rc_test_suite(name):
    """Check resource tool inputs, execution environment, paths, and link metadata."""
    for suffix, test, cross in [("native", _native_test, False), ("cross", _cross_test, True)]:
        fixture = name + "_" + suffix + "_fixture"
        embed_binary_rc(
            name = fixture,
            src = "testdata/empty.wasm",
            out = suffix + "/fixture.res",
            resource_name = "TEST_BINARY",
            rc = "//bazel/toolchains/cc/mongo_windows_cross:windows_rc_test_rc",
            rc_tools = ["//bazel/toolchains/cc/mongo_windows_cross:windows_rc_test_tools"] if cross else [],
            rc_sysroot = ["//bazel/toolchains/cc/mongo_windows_cross:windows_rc_test_sysroot"] if cross else [],
            tags = ["manual"],
        )
        test(name = name + "_" + suffix, target_under_test = ":" + fixture, size = "small")
    native.test_suite(name = name, tests = [name + "_native", name + "_cross"])
