load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts", "unittest")
load("@rules_python//python:py_info.bzl", "PyInfo")
load(":py_action_env.bzl", "py_action_env_pythonpath", "py_runtime_pythonpath")

def _runtime_pythonpath_test_impl(ctx):
    env = unittest.begin(ctx)
    paths = ["bazel-out\\exec\\asn1crypto", "bazel-out\\exec\\cryptography"]
    linux_python = struct(interpreter = struct(extension = ""))
    windows_python = struct(interpreter = struct(extension = "exe"))
    asserts.equals(env, "bazel-out/exec/asn1crypto:bazel-out/exec/cryptography", py_runtime_pythonpath(linux_python, paths))
    asserts.equals(env, "bazel-out/exec/asn1crypto;bazel-out/exec/cryptography", py_runtime_pythonpath(windows_python, paths))
    binary = {
        PyInfo: struct(
            imports = depset(["dependency/site-packages"]),
            transitive_sources = depset([struct(path = "bazel-out/exec/bin/external/dependency/lib.py")]),
        ),
    }
    for runtime, host_separator, expected in [
        (linux_python, ";", "runfiles/package:bazel-out/exec/bin/external/dependency/site-packages"),
        (windows_python, ":", "runfiles/package;bazel-out/exec/bin/external/dependency/site-packages"),
    ]:
        context = struct(
            configuration = struct(host_path_separator = host_separator),
            toolchains = {"@rules_python//python:toolchain_type": struct(py3_runtime = runtime)},
        )
        asserts.equals(env, expected, py_action_env_pythonpath(context, binary, "runfiles/package")["PYTHONPATH"])
    return unittest.end(env)

runtime_pythonpath_test = unittest.make(_runtime_pythonpath_test_impl)

def _template_renderer_python_path_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "TemplateRenderer"
    ]
    asserts.equals(env, 1, len(actions))

    python_path = actions[0].env.get("PYTHONPATH", "")
    asserts.true(
        env,
        "pyyaml@" in python_path and "site-packages" in python_path,
        "TemplateRenderer PYTHONPATH must include the pyyaml wheel import path",
    )
    return analysistest.end(env)

template_renderer_python_path_analysis_test = analysistest.make(
    _template_renderer_python_path_test_impl,
)

def _config_header_linux_python_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "ConfigHeaderGen"
    ]
    asserts.equals(env, 1, len(actions))

    argv = actions[0].argv
    asserts.true(
        env,
        "py_linux_x86_64" in argv[0] and not argv[0].endswith(".exe"),
        "ConfigHeaderGen must use the Linux Python runtime in Windows cross mode",
    )
    asserts.equals(env, "bazel/config/generate_config_header.py", argv[1])
    definitions = json.decode(argv[argv.index("--extra-definitions") + 1])
    asserts.true(env, bool(definitions.get("COMPILER_TYPE")), "ConfigHeaderGen must pass the compiler driver type")
    return analysistest.end(env)

config_header_linux_python_analysis_test = analysistest.make(
    _config_header_linux_python_test_impl,
    config_settings = {
        str(Label("//bazel/config:windows_cross_local_container_actions")): True,
    },
)

def _template_renderer_linux_python_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "TemplateRenderer"
    ]
    asserts.equals(env, 1, len(actions))

    argv = actions[0].argv
    asserts.true(
        env,
        "py_linux_x86_64" in argv[0] and not argv[0].endswith(".exe"),
        "TemplateRenderer must use the Linux Python runtime in Windows cross mode",
    )
    asserts.true(
        env,
        "generate_error_codes.py" in argv[1],
        "TemplateRenderer must invoke the generator source directly in Windows cross mode",
    )
    return analysistest.end(env)

template_renderer_linux_python_analysis_test = analysistest.make(
    _template_renderer_linux_python_test_impl,
    config_settings = {
        str(Label("//bazel/config:windows_cross_local_container_actions")): True,
    },
)

def _python_genrule_action_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [action for action in analysistest.target_actions(env) if action.mnemonic == "TemplateRenderer"]
    asserts.equals(env, 1, len(actions))
    action = actions[0]
    asserts.equals(env, 1, len(action.outputs.to_list()))
    asserts.true(env, action.outputs.to_list()[0].path.endswith("generated_python_input.txt"))
    if ctx.attr.linux_runtime:
        asserts.true(env, "py_linux_x86_64" in action.argv[0])
        asserts.true(env, action.argv[1].endswith("testdata/generator/generate.py"))
        inputs = [file.path for file in action.inputs.to_list()]
        asserts.true(env, any([path.endswith("testdata/generator/generate.py") for path in inputs]))
        asserts.true(env, any(["py_linux_x86_64" in path for path in inputs]))
        runfiles = [file.path for file in ctx.attr.python_binary[DefaultInfo].default_runfiles.files.to_list()]
        asserts.true(env, any([path.endswith("testdata/helper.py") for path in runfiles]), "Tool runfiles must include the helper")
        asserts.true(env, any([path.endswith("testdata/message.txt") for path in runfiles]), "Tool runfiles must include the data")
        asserts.true(env, any([path.endswith("runfiles_generator_python.runfiles") for path in inputs]), "Tool runfiles tree must be an action input")
        asserts.true(env, ".runfiles/_main/bazel/config" in action.env["PYTHONPATH"], "Tool runfiles package must be on PYTHONPATH")
        asserts.true(env, action.env["RUNFILES_DIR"].endswith("runfiles_generator_python.runfiles"))
    else:
        asserts.true(
            env,
            action.argv[0].endswith("runfiles_generator_python") or action.argv[0].endswith("runfiles_generator_python.exe"),
        )
    return analysistest.end(env)

python_genrule_action_analysis_test = analysistest.make(
    _python_genrule_action_test_impl,
    attrs = {"linux_runtime": attr.bool(default = False)},
    config_settings = {
        str(Label("//bazel/config:windows_cross_local_container_actions")): False,
    },
)

python_genrule_linux_python_analysis_test = analysistest.make(
    _python_genrule_action_test_impl,
    attrs = {
        "linux_runtime": attr.bool(default = True),
        "python_binary": attr.label(default = ":runfiles_generator_python", cfg = "exec"),
    },
    config_settings = {
        str(Label("//bazel/config:windows_cross_local_container_actions")): True,
    },
)

def _idl_wrapper_runfiles_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [action for action in analysistest.target_actions(env) if action.mnemonic == "IdlcGenerator"]
    asserts.equals(env, 1, len(actions))
    input_paths = [file.path for file in actions[0].inputs.to_list()]
    asserts.true(
        env,
        any([path.endswith("macos_cross_action_wrapper_sh.runfiles") for path in input_paths]),
        "IDL actions must include the container wrapper's runfiles tree",
    )
    wrapper_runfiles = ctx.attr.wrapper[DefaultInfo].default_runfiles.files.to_list()
    asserts.true(
        env,
        any([file.path == "bazel/toolchains/cc/container_action.py" for file in wrapper_runfiles]),
        "The wrapper's runfiles must include the shared container module",
    )
    return analysistest.end(env)

idl_wrapper_runfiles_analysis_test = analysistest.make(
    _idl_wrapper_runfiles_test_impl,
    attrs = {
        "wrapper": attr.label(
            default = "//bazel/toolchains/cc/mongo_apple_cross:macos_cross_action_wrapper_sh",
            cfg = "exec",
        ),
    },
    config_settings = {
        str(Label("//bazel/config:macos_cross_local_container_actions")): True,
    },
)

def _linux_tool_path_analysis_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == ctx.attr.action_mnemonic
    ]
    asserts.equals(env, 1, len(actions))
    asserts.equals(env, "/usr/bin:/bin", actions[0].env.get("PATH"))
    return analysistest.end(env)

linux_tool_path_analysis_test = analysistest.make(
    _linux_tool_path_analysis_test_impl,
    attrs = {"action_mnemonic": attr.string(mandatory = True)},
    config_settings = {
        "@@//bazel/config:windows_cross_host_path": "C:/Windows;C:/Python",
        "//command_line_option:action_env": ["PATH=/usr/bin:/bin"],
    },
)
