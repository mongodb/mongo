load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _wheel_installer_runfiles_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "WheelInstall"
    ]
    asserts.equals(env, 1, len(actions))

    input_paths = [file.path for file in actions[0].inputs.to_list()]
    has_wheel_installer_runfiles = False
    for path in input_paths:
        if "wheel" in path and path.endswith("runfiles"):
            has_wheel_installer_runfiles = True
            break
    asserts.true(
        env,
        has_wheel_installer_runfiles,
        "WheelInstall must stage the wheel installer runfiles tree",
    )
    if ctx.attr.linux_runtime:
        asserts.equals(env, None, actions[0].env.get("PATH"), "Linux installers must not inherit the target Windows runtime's PATH")
    return analysistest.end(env)

wheel_installer_runfiles_analysis_test = analysistest.make(
    _wheel_installer_runfiles_test_impl,
    attrs = {"linux_runtime": attr.bool(default = False)},
)

wheel_installer_linux_runtime_analysis_test = analysistest.make(
    _wheel_installer_runfiles_test_impl,
    attrs = {"linux_runtime": attr.bool(default = True)},
    config_settings = {
        "//command_line_option:platforms": "@@//bazel/platforms:windows_x86_64_cross",
        "//command_line_option:extra_execution_platforms": ["@@//bazel/platforms:ubuntu24_amd64_windows_cross_exec"],
    },
)
