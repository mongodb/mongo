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
