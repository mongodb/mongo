# Common mongo-specific bazel build rules intended to be used in individual BUILD files in the "src/" subtree.

MONGO_DEFAULT_COPTS = ["-Isrc"]

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
    native.cc_library(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        deps = deps,
        copts = MONGO_DEFAULT_COPTS + copts,
        visibility = visibility,
        testonly = testonly,
        linkopts = linkopts,
        data = data,
        tags = tags,
        linkstatic = linkstatic,
        local_defines = local_defines,
        includes = []
    )
