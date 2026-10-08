load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load(":msi.bzl", "mongo_msi")

def _windows_execution_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = analysistest.target_actions(env)
    asserts.equals(env, 3, len(actions))
    if len(actions) != 3:
        return analysistest.end(env)

    candle, light, trim = actions
    asserts.equals(env, "WixCandle", candle.mnemonic)
    asserts.equals(env, "WixLight", light.mnemonic)
    asserts.equals(env, "MsiTrim", trim.mnemonic)
    asserts.true(env, candle.argv[0].endswith("python.exe"))
    asserts.true(env, candle.argv[2].endswith("candle.exe"))
    custom_action_inputs = [file for file in candle.inputs.to_list() if file.extension == "dll" and file.owner == Label("//src/mongo/installer/msi/ca:mongoca")]
    asserts.equals(env, 1, len(custom_action_inputs))
    if custom_action_inputs:
        asserts.true(env, "-dCustomActionDll=" + custom_action_inputs[0].path in candle.argv)
    asserts.true(env, light.argv[0].endswith("light.exe"))
    asserts.equals(env, candle.argv[0], trim.argv[0])
    asserts.true(env, trim.argv[1].endswith("msitrim.py"))
    asserts.true(env, trim.argv[2].endswith(".pre.msi"))
    asserts.true(env, trim.argv[3].endswith(".msi"))
    asserts.true(env, "PATH" in trim.env)
    asserts.true(env, "PYTHONPATH" in trim.env)
    asserts.true(env, "site-packages" in trim.env.get("PYTHONPATH", ""))
    trim_inputs = trim.inputs.to_list()
    asserts.true(env, trim.argv[1] in [file.path for file in trim_inputs])
    wheel_paths = [file.path for file in trim_inputs if file.is_directory]
    asserts.true(env, any([path in trim.env.get("PYTHONPATH", "") for path in wheel_paths]))
    return analysistest.end(env)

_windows_execution_test = analysistest.make(
    _windows_execution_test_impl,
)

def msi_test_suite(name, custom_action):
    mongo_msi(
        name = name + "_input",
        srcs = [Label("//bazel/install_rules:msi_test.wxs")],
        custom_action = custom_action,
        mongo_version = "8.0.0",
        testonly = True,
        tags = ["manual"],
        target_compatible_with = ["@platforms//os:windows"],
    )
    _windows_execution_test(
        name = name,
        target_under_test = ":" + name + "_input",
        exec_compatible_with = ["@platforms//os:windows"],
        tags = ["mongo_unittest"],
        target_compatible_with = ["@platforms//os:windows"],
    )
