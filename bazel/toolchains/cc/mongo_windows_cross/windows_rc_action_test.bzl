load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")

def _windows_rc_action_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "WindowsRC"
    ]
    asserts.equals(env, 1, len(actions))

    action = actions[0]
    input_paths = [file.path for file in action.inputs.to_list()]
    runtime_library_dirs = []
    archive_library_dirs = []
    for file in action.inputs.to_list():
        if file.basename == "libwindows_rc_test.so":
            runtime_library_dirs.append(file.dirname.replace("\\", "/"))
        if file.basename == "libwindows_rc_test.so.1":
            archive_library_dirs.append(file.dirname.replace("\\", "/"))
    asserts.equals(env, 1, len(runtime_library_dirs), "WindowsRC must stage copied runtime libraries")
    asserts.equals(env, 1, len(archive_library_dirs), "WindowsRC must stage archive runtime libraries")
    asserts.equals(
        env,
        ":".join(runtime_library_dirs + archive_library_dirs),
        action.env.get("LD_LIBRARY_PATH"),
        "WindowsRC must expose copied libraries before archive libraries to llvm-rc and clang",
    )
    has_clang_input = False
    for path in input_paths:
        if "windows_rc_action_test_clang" in path:
            has_clang_input = True
            break
    asserts.true(
        env,
        has_clang_input,
        "WindowsRC must stage clang for llvm-rc preprocessing",
    )
    has_windows_sdk_header = False
    for path in input_paths:
        if "winres.h" in path:
            has_windows_sdk_header = True
            break
    asserts.true(
        env,
        has_windows_sdk_header,
        "WindowsRC must stage Windows SDK headers for resource preprocessing",
    )
    has_windows_sdk_include = False
    for argument in action.argv:
        if "sysroot/winsdk/include/um" in argument or "sysroot\\winsdk\\include\\um" in argument:
            has_windows_sdk_include = True
            break
    asserts.true(
        env,
        has_windows_sdk_include,
        "WindowsRC must pass the Windows SDK include directory to llvm-rc",
    )
    has_generated_include = False
    for argument in action.argv:
        if "bazel-out" in argument and argument.endswith("/src"):
            has_generated_include = True
            break
    asserts.true(
        env,
        has_generated_include,
        "WindowsRC must use a forward-slash generated-source include directory",
    )
    asserts.true(
        env,
        "bazel-out" in action.env.get("PATH", ""),
        "WindowsRC PATH must include the hermetic LLVM tools directory",
    )
    for variable in ["TEMP", "TMP", "TMPDIR"]:
        asserts.equals(
            env,
            "/tmp",
            action.env.get(variable),
            "WindowsRC must use a temporary directory on the Linux worker",
        )
    has_rewritten_source = False
    for path in input_paths:
        if path.endswith("_windows_cross.rc"):
            has_rewritten_source = True
            break
    asserts.true(
        env,
        has_rewritten_source,
        "WindowsRC must rewrite resource paths to staged Bazel inputs",
    )
    return analysistest.end(env)

windows_rc_action_analysis_test = analysistest.make(
    _windows_rc_action_test_impl,
    config_settings = {
        "//command_line_option:platforms": "@@//bazel/platforms:windows_x86_64_cross",
    },
)

def _windows_rc_native_action_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "WindowsRC"
    ]
    asserts.equals(env, 1, len(actions))
    action = actions[0]
    asserts.true(env, action.argv[-1].endswith("windows_rc_action_test.rc"))
    input_paths = [file.path for file in action.inputs.to_list()]
    asserts.false(env, any([path.endswith("_windows_cross.rc") for path in input_paths]))
    for variable in ["PATH", "LD_LIBRARY_PATH", "TEMP", "TMP", "TMPDIR"]:
        asserts.false(env, variable in action.env, "Native WindowsRC must retain its environment")
    return analysistest.end(env)

windows_rc_native_action_analysis_test = analysistest.make(
    _windows_rc_native_action_test_impl,
    config_settings = {
        "//command_line_option:platforms": "@@//bazel/platforms:windows_amd64",
    },
)
