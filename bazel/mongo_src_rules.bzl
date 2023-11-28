# Common mongo-specific bazel build rules intended to be used in individual BUILD files in the "src/" subtree.
load("@aspect_bazel_lib//lib:expand_make_vars.bzl", "expand_locations")
# === Windows-specific compilation settings ===

# /RTC1              Enable Stack Frame Run-Time Error Checking; Reports when a variable is used without having been initialized (implies /Od: no optimizations)
# /O1                optimize for size
# /O2                optimize for speed (as opposed to size)
# /Oy-               disable frame pointer optimization (overrides /O2, only affects 32-bit)
# /INCREMENTAL: NO - disable incremental link - avoid the level of indirection for function calls

WINDOWS_DBG_COPTS = [
    "/MDd",
    "/RTC1",
    "/Od",
]
WINDOWS_OPT_ON_COPTS = [
    "/MD",
    "/O2",
    "/Oy-",
]
WINDOWS_OPT_OFF_COPTS = [
    "-O0",
]

WINDOWS_OPT_DBG_COPTS = [
    "/MD",
    "/RTC1",
    "/Ox",
    "/Zo",
    "/Oy-",
]
WINDOWS_OPT_SIZE_COPTS = [
    "/MD",
    "/Os",
    "/Oy-",
]
WINDOWS_RELEASE_COPTS = [
    "/MD",
    "/Od",
]

MONGO_GLOBAL_COPTS = ["-Isrc"] + select({
    "//bazel/config:windows_dbg": WINDOWS_DBG_COPTS,
    "//bazel/config:windows_opt_on": WINDOWS_OPT_ON_COPTS,
    "//bazel/config:windows_opt_off": WINDOWS_OPT_OFF_COPTS,
    "//bazel/config:windows_opt_debug": WINDOWS_OPT_DBG_COPTS,
    "//bazel/config:windows_opt_size": WINDOWS_OPT_SIZE_COPTS,
    "//bazel/config:windows_release": WINDOWS_RELEASE_COPTS,
    "//conditions:default": [],
})

MONGO_GLOBAL_DEFINES = select({
    "//bazel/config:dbg": ["MONGO_CONFIG_DEBUG_BUILD"],
    "//conditions:default": ["NDEBUG"],
})

LIBUNWIND_DEPS = select({
    "//bazel/config:use_libunwind_enabled": ["//src/third_party/unwind:unwind"],
    "//conditions:default": [],
})

LIBUNWIND_DEFINES = select({
    "//bazel/config:use_libunwind_enabled": ["MONGO_CONFIG_USE_LIBUNWIND"],
    "//conditions:default": [],
})

def mongo_cc_library(
        name,
        srcs = [],
        hdrs = [],
        deps = [],
        testonly = False,
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        linkstatic = False,
        local_defines = []):
    """Wrapper around cc_library.

    Args:
      name: The name of the library the target is compiling.
      srcs: The source files to build.
      hdrs: The headers files of the target library.
      deps: The targets the library depends on.
      testonly: Whether or not the target is purely for tests.
      visibility: The visibility of the target library.
      data: Data targets the library depends on.
      tags: Tags to add to the rule.
      copts: Any extra compiler options to pass in.
      linkopts: Any extra link options to pass in.
      linkstatic: Whether or not linkstatic should be passed to the native bazel cc_test rule. This argument
        is ignored on windows since linking into DLLs is not currently supported.
      local_defines: macro definitions passed to all source and header files.
    """

    # Avoid injecting into unwind/libunwind_asm to avoid a circular dependency.
    if name not in ["unwind", "libunwind_asm"]:
        deps += LIBUNWIND_DEPS
        local_defines += LIBUNWIND_DEFINES

    native.cc_library(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        deps = deps,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_GLOBAL_COPTS + copts,
        data = data,
        tags = tags,
        linkstatic = select({
            "@platforms//os:windows": True,
            "//conditions:default": linkstatic,
        }),
        local_defines = MONGO_GLOBAL_DEFINES + local_defines,
        includes = [],
    )

def mongo_cc_binary(
        name,
        srcs = [],
        deps = [],
        testonly = False,
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        linkstatic = False,
        local_defines = []):
    """Wrapper around cc_binary.

    Args:
      name: The name of the library the target is compiling.
      srcs: The source files to build.
      deps: The targets the library depends on.
      testonly: Whether or not the target is purely for tests.
      visibility: The visibility of the target library.
      data: Data targets the library depends on.
      tags: Tags to add to the rule.
      copts: Any extra compiler options to pass in.
      linkopts: Any extra link options to pass in.
      linkstatic: Whether or not linkstatic should be passed to the native bazel cc_test rule. This argument
        is ignored on windows since linking into DLLs is not currently supported.
      local_defines: macro definitions passed to all source and header files.
    """

    native.cc_binary(
        name = name,
        srcs = srcs,
        deps = deps + LIBUNWIND_DEPS,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_GLOBAL_COPTS + copts,
        data = data,
        tags = tags,
        linkstatic = select({
            "@platforms//os:windows": True,
            "//conditions:default": linkstatic,
        }),
        local_defines = MONGO_GLOBAL_DEFINES + LIBUNWIND_DEFINES + local_defines,
        malloc = select({
          "//bazel/config:tcmalloc_allocator": "//src/third_party/gperftools:tcmalloc_minimal",
          "//bazel/config:auto_allocator_windows": "//src/third_party/gperftools:tcmalloc_minimal",
          "//bazel/config:auto_allocator_linux": "//src/third_party/gperftools:tcmalloc_minimal",
          "//conditions:default": "@bazel_tools//tools/cpp:malloc",
        }),
        includes = [],
    )


# This is an example of running a python script for a build where its expected some output
# of the script will then be the input to another part of the build, source code generation
# for example. Technically we could use py_binary + genrule (or even just genrule), but 
# interface is so generic that it become complex to create such genrules, and you
# end up writing a bunch of implementation logic into the build file.
# This rule is in itself generic as well and may be subject for removal in the future
# depending on uses cases and or other rules that are implemented.
def run_python_buildscript_impl(ctx):
    
    # will use this to get a path to the interpreter
    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime

    # collect deps from and extra python files.
    src_depsets = [src.files for src in ctx.attr.srcs]

    # collect deps from python modules and setup the corresponding
    # path so all modules can be found by the toolchain.
    python_path = []
    for py_dep in ctx.attr.py_deps:
        for dep in py_dep[PyInfo].transitive_sources.to_list():
            if dep.path not in  python_path:
                python_path.append(dep.path)
    py_depsets = [py_dep[PyInfo].transitive_sources for py_dep in ctx.attr.py_deps]

    # aggregate the deps to a single var
    inputs = depset(transitive=[
        ctx.attr.script.files,
        python.files] + src_depsets + py_depsets)

    # resolve and bazel expansion that may be present in the args
    args_resolved = [] 
    for arg in ctx.attr.args:
        args_resolved.append(expand_locations(ctx, arg))

    # Run it!
    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = ctx.outputs.outputs,
        inputs = inputs,
        arguments = [ctx.attr.script.files.to_list()[0].path] + args_resolved,
        mnemonic = "PythonScriptRunner",
        env={"PYTHONPATH":':'.join(python_path)}
    )
    
    return [
        DefaultInfo(
            files = depset(ctx.outputs.outputs),
        ),
    ]

run_python_buildscript = rule(
    run_python_buildscript_impl,
    attrs = {
        "script": attr.label(
            doc = "The script to execute.",
            allow_single_file=True,
        ),
        "args" : attr.string_list(
            doc = "Args to pass to the script."    
        ),
        "srcs" : attr.label_list(
            doc = "Supporting scripts which may be imported.",
            allow_files=True,
        ),
        "outputs" : attr.output_list(
            doc = "Output files that will be generated by this script.",
        ),
        "py_deps" : attr.label_list(
            doc = "Python modules that should be imported.",
            providers = [PyInfo],
        ),
    },
    doc = "Run a python script that may import modules.",
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
    fragments = ["py"]
)