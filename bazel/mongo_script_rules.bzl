"""Common mongo-specific bazel build rules intended to be used for buildscripts.
"""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("@rules_python//python:py_info.bzl", "PyInfo")

MONGO_TOOLCHAIN_V4_PATH = "/opt/mongodbtoolchain/v4"

# Sentinel for `toolchain_path`: the mongo toolchain repo is created by a module
# extension, so its directory is the canonical (mangled) name — not
# "external/mongo_toolchain_v5". Rather than spelling that out, resolve it at
# analysis time from the compiler the cc toolchain actually selected.
MONGO_TOOLCHAIN_V5_PATH = "{derive_from_cc_toolchain}"

_RUNFILES_PREFIX = "${RUNFILES_DIR}/"

def _runfiles_path(file, workspace_name):
    # short_path is "../<repo>/<path>" for another repo, "<path>" for this one.
    if file.short_path.startswith("../"):
        return _RUNFILES_PREFIX + file.short_path[len("../"):]
    return _RUNFILES_PREFIX + workspace_name + "/" + file.short_path

def _resolve_toolchain_path(toolchain_path, cc_toolchain):
    if toolchain_path != MONGO_TOOLCHAIN_V5_PATH:
        return toolchain_path

    # compiler_executable is "external/<repo>/<version>/bin/clang" — strip
    # "bin/clang" to get the toolchain root, then rebase it onto the runfiles
    # tree, which is what the test actually runs against.
    toolchain_root = cc_toolchain.compiler_executable.rsplit("/", 2)[0]
    return _RUNFILES_PREFIX + toolchain_root[len("external/"):]

def _py_cxx_wrapper(*, python_path, toolchain_path, python_interpreter, main_py):
    return "\n".join([
        "export PYTHONPATH={}".format(python_path),
        "export MONGO_TOOLCHAIN_PATH={}".format(toolchain_path),
        "{} {}".format(python_interpreter, main_py),
    ])

def _py_cxx_test_impl(ctx):
    python = ctx.toolchains["@rules_python//python:toolchain_type"].py3_runtime

    python_path = []
    for dep in ctx.attr.deps:
        for path in dep[PyInfo].imports.to_list():
            if path not in python_path:
                python_path.append(
                    ctx.expand_make_variables(
                        "python_library_imports",
                        "$${RUNFILES_DIR}/" + path,
                        ctx.var,
                    ),
                )
    python_path_str = ctx.configuration.host_path_separator.join(python_path)

    cc_toolchain = find_cpp_toolchain(ctx)
    runfiles = ctx.runfiles(
        files = (
            ctx.files.srcs +
            ctx.files.data +
            ctx.files.deps +
            ctx.files.main +
            python.files.to_list() +
            cc_toolchain.all_files.to_list()
        ),
    )
    transitive_runfiles = []
    for runfiles_attr in (
        [ctx.attr.main],
        ctx.attr.srcs,
        ctx.attr.deps,
        ctx.attr.data,
    ):
        for target in runfiles_attr:
            transitive_runfiles.append(target[DefaultInfo].default_runfiles)
    runfiles = runfiles.merge_all(transitive_runfiles)

    script = _py_cxx_wrapper(
        python_path = python_path_str,
        toolchain_path = _resolve_toolchain_path(ctx.attr.toolchain_path, cc_toolchain),
        python_interpreter = _runfiles_path(python.interpreter, ctx.workspace_name),
        main_py = _runfiles_path(ctx.files.main[0], ctx.workspace_name),
    )
    ctx.actions.write(
        output = ctx.outputs.executable,
        content = script,
    )

    return DefaultInfo(files = depset([ctx.outputs.executable]), runfiles = runfiles)

py_cxx_test = rule(
    implementation = _py_cxx_test_impl,
    attrs = {
        "main": attr.label(allow_single_file = True, mandatory = True),
        "srcs": attr.label_list(allow_files = [".py"]),
        "deps": attr.label_list(),
        "data": attr.label_list(),
        "toolchain_path": attr.string(mandatory = True),
    },
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type", "@rules_python//python:toolchain_type"],
    executable = True,
    test = True,
)

def mongo_toolchain_py_cxx_test(**kwargs):
    py_cxx_test(
        toolchain_path = select({
            "//bazel/config:mongo_toolchain_v5": MONGO_TOOLCHAIN_V5_PATH,
            "//conditions:default": MONGO_TOOLCHAIN_V4_PATH,
        }),
        target_compatible_with = ["@//bazel/platforms:use_mongo_toolchain"],
        **kwargs
    )
