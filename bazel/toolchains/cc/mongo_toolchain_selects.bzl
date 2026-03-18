"""Common select() definitions shared across toolchain BUILD templates."""

DEBUG_LEVEL = select({
    "@//bazel/config:gcc_or_clang_dbg_level_0": 0,
    "@//bazel/config:gcc_or_clang_dbg_level_1": 1,
    "@//bazel/config:gcc_or_clang_dbg_level_2": 2,
    "@//bazel/config:gcc_or_clang_dbg_level_3": 3,
})

INTERNAL_THIN_LTO_ENABLED = select({
    "@//bazel/config:thin_lto_enabled": True,
    "@//conditions:default": False,
})

COVERAGE_ENABLED = select({
    "@//bazel/config:gcov_enabled": True,
    "@//conditions:default": False,
})

COMPRESS_DEBUG_ENABLED = select({
    "@//bazel/config:compress_debug_compile_enabled": True,
    "@//conditions:default": False,
})

WARNINGS_AS_ERRORS_ENABLED = select({
    "@//bazel/config:warnings_as_errors_enabled": True,
    "@//conditions:default": False,
})

LINKSTATIC_ENABLED = select({
    "@//bazel/config:linkstatic_enabled": True,
    "@//conditions:default": False,
})
