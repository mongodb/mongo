# Common mongo-specific bazel build rules intended to be used in individual BUILD files in the "src/" subtree.

MONGO_DEFAULT_COPTS = ["-Isrc"]

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
      linkstatic: Whether or not linkstatic should be passed to the native bazel cc_library rule.
      local_defines: macro definitions passed to all source and header files.
    """

    global_deps = []
    global_defines = []

    # Avoid injecting into unwind/libunwind_asm to avoid a circular dependency.
    if name not in ["unwind", "libunwind_asm"]:
        global_deps = select({
            "//bazel/config:use_libunwind_enabled": ["//src/third_party/unwind:unwind"],
            "//conditions:default": [],
        })
        global_defines = select({
            "//bazel/config:use_libunwind_enabled": ["MONGO_CONFIG_USE_LIBUNWIND"],
            "//conditions:default": [],
        })

    native.cc_library(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        deps = deps + global_deps,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_DEFAULT_COPTS + copts +
                select({
                    "//bazel/config:windows_dbg": WINDOWS_DBG_COPTS,
                    "//bazel/config:windows_opt_on": WINDOWS_OPT_ON_COPTS,
                    "//bazel/config:windows_opt_off": WINDOWS_OPT_OFF_COPTS,
                    "//bazel/config:windows_opt_debug": WINDOWS_OPT_DBG_COPTS,
                    "//bazel/config:windows_opt_size": WINDOWS_OPT_SIZE_COPTS,
                    "//bazel/config:windows_release": WINDOWS_RELEASE_COPTS,
                    "//conditions:default": [],
                }),
        data = data,
        tags = tags,
        linkstatic = linkstatic,
        local_defines = local_defines + global_defines +
                        select({
                            "//bazel/config:dbg": ["MONGO_CONFIG_DEBUG_BUILD"],
                            "//conditions:default": ["NDEBUG"],
                        }),
        includes = [],
    )
