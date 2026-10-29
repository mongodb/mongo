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
    return analysistest.end(env)

wheel_installer_runfiles_analysis_test = analysistest.make(
    _wheel_installer_runfiles_test_impl,
)
