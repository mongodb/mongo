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

load("@rules_python//python:py_executable_info.bzl", "PyExecutableInfo")
load("@rules_python//python:py_info.bzl", "PyInfo")

def py_runtime_pythonpath(python, paths):
    """Format import paths for the selected interpreter's execution OS.

    host_path_separator describes the Bazel client, which can be Windows
    while the selected Python runtime executes on a Linux remote worker.
    """
    separator = ";" if python.interpreter.extension == "exe" else ":"
    return separator.join([path.replace("\\", "/") for path in paths])

def py_linux_python_action(ctx, python_binary, python_interpreter_target, python_files_target):
    """Returns the Linux Python action configuration for a cross build.

    A py_binary selected for a Windows execution platform produces a Windows
    launcher. Cross-build generator actions run in the Linux RBE container, so
    invoke the py_binary's source with the downloaded Linux Python runtime
    instead of passing the Windows launcher to the remote worker.
    """
    linux_python_interpreters = python_interpreter_target.files.to_list()
    if len(linux_python_interpreters) != 1:
        fail("Linux Python runtime was requested but is not available")

    python_interpreter = linux_python_interpreters[0].path.replace("\\", "/")
    python_files = python_files_target.files

    # The interpreter label may resolve to a symlink such as `python3`. Use the
    # versioned executable from the extracted distribution so the action remains
    # runnable when Bazel materializes the Linux runtime on a Windows host.
    for python_file in python_files.to_list():
        if python_file.basename.startswith("python3.") and not python_file.basename.endswith("-config"):
            python_interpreter = python_file.path.replace("\\", "/")
            break

    python_source = python_binary[PyExecutableInfo].main
    if not python_source:
        fail("Python generator must provide a main source")

    python_path = py_exec_import_paths(ctx, [python_binary])
    action_env = {
        # The action executes in the Linux RBE container even though analysis is
        # performed by a Windows host Bazel. Use Linux path syntax throughout.
        "PYTHONPATH": ":".join(
            [python_source.dirname.replace("\\", "/")] +
            [path.replace("\\", "/") for path in python_path],
        ),
    }
    return python_interpreter, python_source, python_files, action_env

def py_action_env_windows_dll_path(ctx):
    """Returns the Windows Python DLL and standard-library environment.

    On Windows, returns `PATH` and `PYTHONHOME` set to the Python distribution
    directory; on other platforms returns `{}`.

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
    #
    # PYTHONHOME is also needed because rules_python can launch the interpreter
    # through a manifest-only runfiles alias. Without it, CPython derives its
    # stdlib location from that alias and cannot find encodings before the
    # action's PYTHONPATH is processed.
    return {
        "PATH": interpreter.dirname,
        "PYTHONHOME": interpreter.dirname,
    }

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

def py_action_env_pythonpath(ctx, python_binary, runfiles_package_dir):
    """Returns a PYTHONPATH containing a Python binary and its dependencies.

    Custom actions that invoke a py_binary directly do not get the import path
    setup performed by the standard rules_python launcher. Include the binary's
    runfiles package directory and its exec-configured dependency imports so
    those actions work when Windows runfiles are manifest-only.
    """
    python_path = [runfiles_package_dir] + py_exec_import_paths(ctx, [python_binary])
    python = ctx.toolchains["@rules_python//python:toolchain_type"].py3_runtime
    return {
        "PYTHONPATH": py_runtime_pythonpath(python, python_path),
    }
