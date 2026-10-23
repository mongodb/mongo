"""Check that build metadata probes the selected compile and link actions."""

load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

_LinkActionsInfo = provider(fields = ["actions"])

def _link_actions_aspect_impl(target, ctx):
    actions = list(target.actions)
    if hasattr(ctx.rule.attr, "srcs"):
        for dependency in ctx.rule.attr.srcs:
            if _LinkActionsInfo in dependency:
                actions += dependency[_LinkActionsInfo].actions
    return [_LinkActionsInfo(actions = actions)]

_link_actions_aspect = aspect(
    implementation = _link_actions_aspect_impl,
    attr_aspects = ["srcs"],
)

def _linker_features_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = analysistest.target_under_test(env)[_LinkActionsInfo].actions
    probes = [action for action in actions if action.argv and "--kind" in action.argv and action.argv[action.argv.index("--kind") + 1] == "linker"]
    links = [action for action in actions if action.mnemonic == "CppLink" and action.argv and "--kind" not in action.argv]
    asserts.equals(env, 1, len(probes))
    asserts.equals(env, 1, len(links))
    probe_flags = json.decode(probes[0].argv[probes[0].argv.index("--flags") + 1])
    for flags in [probe_flags, links[0].argv]:
        selection = [flag for flag in flags if flag.startswith("-fuse-ld=")]
        asserts.equals(env, ["-fuse-ld=mold"] if ctx.attr.mold else [], selection)
    return analysistest.end(env)

requested_linker_features_analysis_test = analysistest.make(
    _linker_features_test_impl,
    attrs = {"mold": attr.bool(default = True)},
    extra_target_under_test_aspects = [_link_actions_aspect],
    config_settings = {"//command_line_option:features": ["linker_mold"]},
)

disabled_linker_features_analysis_test = analysistest.make(
    _linker_features_test_impl,
    attrs = {"mold": attr.bool(default = False)},
    extra_target_under_test_aspects = [_link_actions_aspect],
    config_settings = {"//command_line_option:features": ["-default_linker_lld", "-default_linker_mold"]},
)

def _local_cross_link_inputs_test_impl(ctx):
    env = analysistest.begin(ctx)
    probes = [action for action in analysistest.target_actions(env) if action.argv and "--kind" in action.argv and action.argv[action.argv.index("--kind") + 1] == "linker"]
    asserts.equals(env, 1, len(probes))
    inputs = {file.path: True for file in probes[0].inputs.to_list()}
    missing = [file.path for file in ctx.attr.host_linker_files.files.to_list() if file.path not in inputs]
    asserts.equals(env, [], missing[:5], "Missing native host linker inputs (first five)")
    return analysistest.end(env)

local_cross_link_inputs_analysis_test = analysistest.make(
    _local_cross_link_inputs_test_impl,
    attrs = {"host_linker_files": attr.label(default = "@mongo_toolchain_v5//:all_files")},
    config_settings = {"//command_line_option:extra_toolchains": [str(Label("//bazel/config:probe_test_toolchain"))]},
)

def _version_actions_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = analysistest.target_actions(env)
    probes = [
        action
        for action in actions
        if any([file.basename.endswith("_version.json") for file in action.outputs.to_list()])
    ]
    asserts.equals(env, 2, len(probes))
    for action in probes:
        compiler = any([file.basename.endswith("_compiler_version.json") for file in action.outputs.to_list()])
        asserts.equals(env, "ConfigHeaderGen" if compiler else "CppLink", action.mnemonic)
        asserts.true(env, "--driver" in action.argv)
        if not compiler:
            flags = action.argv[action.argv.index("--flags") + 1]
            asserts.true(env, "-fuse-ld=" in flags)
    generator = [action for action in actions if any([file.basename == "toolchain_provenance.json" for file in action.outputs.to_list()])][0]
    inputs = [file.basename for file in generator.inputs.to_list()]
    for required in ["toolchain_provenance_compiler_version.json", "toolchain_provenance_linker_version.json"]:
        asserts.true(env, required in inputs, "Missing version-generator input " + required)
    asserts.equals(env, "ConfigHeaderGen", generator.mnemonic)
    argv = generator.argv
    asserts.equals(env, "macOS" if ctx.attr.macos_cross else "linux", argv[argv.index("--target-os") + 1])
    if ctx.attr.macos_cross:
        asserts.equals(env, "arm64", argv[argv.index("--target-arch") + 1])
        asserts.equals(env, "cross", argv[argv.index("--toolchain-mode") + 1])
    return analysistest.end(env)

toolchain_versions_analysis_test = analysistest.make(
    _version_actions_test_impl,
    attrs = {"macos_cross": attr.bool(default = False)},
)

macos_cross_toolchain_versions_analysis_test = analysistest.make(
    _version_actions_test_impl,
    attrs = {"macos_cross": attr.bool(default = True)},
    config_settings = {
        "//command_line_option:platforms": str(Label("//bazel/platforms:macos_arm64_cross")),
    },
)

def _macos_cross_compile_flags_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = analysistest.target_under_test(env)[_LinkActionsInfo].actions
    compiles = [action for action in actions if action.mnemonic == "CppCompile"]
    asserts.equals(env, 1, len(compiles))
    flags = compiles[0].argv
    asserts.true(env, "-Werror" in flags, "Cross builds must retain warnings as errors")
    asserts.false(env, "-Wno-character-conversion" in flags, "LLVM 19 does not support this warning")
    return analysistest.end(env)

macos_cross_compile_flags_analysis_test = analysistest.make(
    _macos_cross_compile_flags_test_impl,
    extra_target_under_test_aspects = [_link_actions_aspect],
    config_settings = {
        "//command_line_option:platforms": str(Label("//bazel/platforms:macos_arm64_cross")),
    },
)

def _macos_cross_probe_runtime_test_impl(ctx):
    env = analysistest.begin(ctx)
    probes = [action for action in analysistest.target_actions(env) if action.argv and "--kind" in action.argv]
    asserts.equals(env, 2, len(probes))
    for action in probes:
        kind = action.argv[action.argv.index("--kind") + 1]
        if kind == "linker" and ctx.attr.remote_link:
            interpreter = ctx.attr.linux_python_interpreter.files.to_list()[0]
            asserts.equals(env, interpreter.path, action.argv[0])
            asserts.equals(env, "bazel/toolchains/cc/toolchain_version_probe.py", action.argv[1])
            inputs = {file.path: True for file in action.inputs.to_list()}
            missing = [file.path for file in ctx.attr.linux_python_files.files.to_list() if file.path not in inputs]
            asserts.equals(env, [], missing[:5], "Missing Linux Python runtime inputs (first five)")
            asserts.true(env, action.argv[1] in inputs)
        else:
            # Local probes retain the execution-platform py_binary launcher.
            asserts.true(env, action.argv[0].endswith("/toolchain_version_probe"))
    return analysistest.end(env)

macos_cross_remote_link_runtime_analysis_test = analysistest.make(
    _macos_cross_probe_runtime_test_impl,
    attrs = {
        "remote_link": attr.bool(default = True),
        "linux_python_interpreter": attr.label(default = "@py_linux_arm64//:interpreter"),
        "linux_python_files": attr.label(default = "@py_linux_arm64//:files"),
    },
    config_settings = {
        "//command_line_option:platforms": str(Label("//bazel/platforms:macos_arm64_cross")),
        str(Label("//bazel/config:macos_cross_local_container_actions")): True,
        str(Label("//bazel/config:remote_link")): True,
    },
)

macos_cross_local_link_runtime_analysis_test = analysistest.make(
    _macos_cross_probe_runtime_test_impl,
    attrs = {"remote_link": attr.bool(default = False)},
    config_settings = {
        "//command_line_option:platforms": str(Label("//bazel/platforms:macos_arm64_cross")),
        str(Label("//bazel/config:macos_cross_local_container_actions")): True,
        str(Label("//bazel/config:remote_link")): False,
    },
)
