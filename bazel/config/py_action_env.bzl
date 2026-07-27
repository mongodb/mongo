"""Helpers for rules that run Python tools as build actions.

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

load("@rules_python//python:py_info.bzl", "PyInfo")

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

def py_exec_import_paths(ctx, py_deps):
    """Exec-tree PYTHONPATH entries for exec-configured python deps.

    `PyInfo.imports` entries are repo-relative runfiles short-paths (e.g.
    "rules_pycross~~lock_repos~pypi/_lock/pymongo@4.12.0/site-packages").
    Converting one into an exec-tree path requires prepending the bindir the
    dep was actually built under, plus "/external/". The naive
    `$(BINDIR)/external/<import>` resolves $(BINDIR) to the consuming rule's
    *target* bindir — wrong whenever the dep attr uses `cfg = "exec"` (the
    files live under `bazel-out/<host>-opt-exec-ST-.../bin`), and wrong in a
    way that only fails loudly under cross-compilation, when the target
    config selects wheels for a different OS/arch than the machine executing
    the action. Derive the correct prefix from a real transitive source's
    path instead — all files in an exec-configured PyInfo share the same
    `bazel-out/.../bin/external/` ancestor.

    Args:
        ctx: The rule context.
        py_deps: List of exec-configured python dependency Targets
            (providing PyInfo).

    Returns:
        A deduplicated list of exec-tree import paths for PYTHONPATH.
    """
    external_marker = "/external/"
    exec_external_prefix = None
    for py_dep in py_deps:
        for f in py_dep[PyInfo].transitive_sources.to_list():
            idx = f.path.find(external_marker)
            if idx >= 0:
                exec_external_prefix = f.path[:idx + len(external_marker)]
                break
        if exec_external_prefix:
            break

    python_path = []
    for py_dep in py_deps:
        for path in py_dep[PyInfo].imports.to_list():
            if exec_external_prefix:
                candidate = exec_external_prefix + path
            else:
                # Fallback for the unlikely case of no transitive_sources
                # (e.g. a py_dep with only imports metadata). Use target
                # bindir; this matches pre-`cfg=exec` behavior.
                candidate = ctx.expand_make_variables(
                    "python_library_imports",
                    "$(BINDIR)/external/" + path,
                    ctx.var,
                )
            if candidate not in python_path:
                python_path.append(candidate)
    return python_path
