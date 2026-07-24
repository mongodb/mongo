"""Helper for injecting Python DLL directory into action env on Windows.

Under rules_python 2.x on Windows, py_binary targets are launched via a
Bazel-emitted `.exe` stub that statically links to the Python C API. When
that launcher starts, the Windows image loader must resolve `python313.dll`
before the launcher's own `main()` runs — otherwise the process exits with
`STATUS_DLL_NOT_FOUND` (0xC0000135, seen as `Exit -1073741515` in Bazel logs).

The DLL is co-located with `python.exe` in mongo's python-build-standalone
toolchain (`dist/python.exe` + `dist/python313.dll`). For the loader to find
it, that directory must be on the process's DLL search path, which on
Windows means the `PATH` env var.

`ctx.actions.run(env = ...)` fully replaces the default action environment,
so `--action_env=PATH` from `.bazelrc` does *not* reach these actions. Fix
must happen at the rule level, where the env dict is built.

Usage:

    load("//bazel/config:py_action_env.bzl", "py_action_env_windows_dll_path")

    def _my_impl(ctx):
        env = {"PYTHONPATH": ...}
        env.update(py_action_env_windows_dll_path(ctx))
        ctx.actions.run(env = env, ...)

    my_rule = rule(
        implementation = _my_impl,
        toolchains = ["@rules_python//python:toolchain_type"],
        ...
    )
"""

def py_action_env_windows_dll_path(ctx):
    """Returns `{"PATH": <python_dir>}` on Windows, else `{}`.

    The rule using this helper must declare
    `toolchains = ["@rules_python//python:toolchain_type"]`.

    Args:
        ctx: The rule context.

    Returns:
        A dict suitable for merging into an action's `env`.
    """
    toolchain = ctx.toolchains["@rules_python//python:toolchain_type"]
    if not toolchain or not toolchain.py3_runtime:
        return {}
    interpreter = toolchain.py3_runtime.interpreter
    if not interpreter or not interpreter.basename.endswith(".exe"):
        return {}

    # `interpreter.dirname` is execroot-relative, e.g.
    # `external/py_windows_x86_64/dist`. Bazel sets the action's CWD to
    # the execroot on Windows, so the launcher's DLL search finds
    # `python313.dll` in this directory.
    return {"PATH": interpreter.dirname}
