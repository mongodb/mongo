"""Abseil compiler options.

This is the source of truth for Abseil compiler options.  To modify Abseil
compilation options:

  (1) Edit the appropriate list in this file based on the platform the flag is
      needed on.
  (2) Run `<path_to_absl>/copts/generate_copts.py`.

The generated copts are consumed by configure_copts.bzl and
AbseilConfigureCopts.cmake.
"""

# /Wall with msvc includes unhelpful warnings such as C4711, C4710, ...
MSVC_BIG_WARNING_FLAGS = [
    "/W3",
]

LLVM_TEST_DISABLE_WARNINGS_FLAGS = [
    "-Wno-c99-extensions",
    "-Wno-deprecated-declarations",
    "-Wno-missing-noreturn",
    "-Wno-missing-prototypes",
    "-Wno-missing-variable-declarations",
    "-Wno-null-conversion",
    "-Wno-shadow",
    "-Wno-shift-sign-overflow",
    "-Wno-sign-compare",
    "-Wno-unused-function",
    "-Wno-unused-member-function",
    "-Wno-unused-parameter",
    "-Wno-unused-private-field",
    "-Wno-unused-template",
    "-Wno-used-but-marked-unused",
    "-Wno-zero-as-null-pointer-constant",
    # gtest depends on this GNU extension being offered.
    "-Wno-gnu-zero-variadic-macro-arguments",
]

MSVC_DEFINES = [
    "/DNOMINMAX",  # Don't define min and max macros (windows.h)
    # Don't bloat namespace with incompatible winsock versions.
    "/DWIN32_LEAN_AND_MEAN",
    # Don't warn about usage of insecure C functions.
    "/D_CRT_SECURE_NO_WARNINGS",
    "/D_SCL_SECURE_NO_WARNINGS",
    # Introduced in VS 2017 15.8, allow overaligned types in aligned_storage
    "/D_ENABLE_EXTENDED_ALIGNED_STORAGE",
]

COPT_VARS = {
    "ABSL_GCC_FLAGS": [
        "-Wall",
        "-Wextra",
        "-Wcast-qual",
        "-Wconversion-null",
        "-Wformat-security",
        "-Wmissing-declarations",
        "-Woverlength-strings",
        "-Wpointer-arith",
        "-Wundef",
        "-Wunused-local-typedefs",
        "-Wunused-result",
        "-Wvarargs",
        "-Wvla",  # variable-length array
        "-Wwrite-strings",
        # Don't define min and max macros (Build on Windows using gcc)
        "-DNOMINMAX",
    ],
    "ABSL_GCC_TEST_FLAGS": [
        "-Wno-conversion-null",
        "-Wno-deprecated-declarations",
        "-Wno-missing-declarations",
        "-Wno-sign-compare",
        "-Wno-unused-function",
        "-Wno-unused-parameter",
        "-Wno-unused-private-field",
    ],
    "ABSL_LLVM_FLAGS": [
        "-Wall",
        "-Wextra",
        "-Wcast-qual",
        "-Wconversion",
        "-Wfloat-overflow-conversion",
        "-Wfloat-zero-conversion",
        "-Wfor-loop-analysis",
        "-Wformat-security",
        "-Wgnu-redeclared-enum",
        "-Winfinite-recursion",
        "-Wliteral-conversion",
        "-Wmissing-declarations",
        "-Woverlength-strings",
        "-Wpointer-arith",
        "-Wself-assign",
        "-Wshadow",
        "-Wstring-conversion",
        "-Wtautological-overlap-compare",
        "-Wundef",
        "-Wuninitialized",
        "-Wunreachable-code",
        "-Wunused-comparison",
        "-Wunused-local-typedefs",
        "-Wunused-result",
        "-Wvla",
        "-Wwrite-strings",
        # Warnings that are enabled by group warning flags like -Wall that we
        # explicitly disable.
        "-Wno-float-conversion",
        "-Wno-implicit-float-conversion",
        "-Wno-implicit-int-float-conversion",
        "-Wno-implicit-int-conversion",
        "-Wno-shorten-64-to-32",
        "-Wno-sign-conversion",
        # Don't define min and max macros (Build on Windows using clang)
        "-DNOMINMAX",
    ],
    "ABSL_LLVM_TEST_FLAGS":
        LLVM_TEST_DISABLE_WARNINGS_FLAGS,
    "ABSL_CLANG_CL_FLAGS":
        (MSVC_BIG_WARNING_FLAGS + MSVC_DEFINES),
    "ABSL_CLANG_CL_TEST_FLAGS":
        LLVM_TEST_DISABLE_WARNINGS_FLAGS,
    "ABSL_MSVC_FLAGS":
        MSVC_BIG_WARNING_FLAGS + MSVC_DEFINES + [
            # Increase the number of sections available in object files
            "/bigobj",
            "/wd4005",  # macro-redefinition
            "/wd4068",  # unknown pragma
            # qualifier applied to function type has no meaning; ignored
            "/wd4180",
            # conversion from 'type1' to 'type2', possible loss of data
            "/wd4244",
            # conversion from 'size_t' to 'type', possible loss of data
            "/wd4267",
            # The decorated name was longer than the compiler limit
            "/wd4503",
            # forcing value to bool 'true' or 'false' (performance warning)
            "/wd4800",
        ],
    "ABSL_MSVC_TEST_FLAGS": [
        "/wd4018",  # signed/unsigned mismatch
        "/wd4101",  # unreferenced local variable
        "/wd4503",  # decorated name length exceeded, name was truncated
        "/wd4996",  # use of deprecated symbol
        "/DNOMINMAX",  # disable the min() and max() macros from <windows.h>
    ],
    "ABSL_MSVC_LINKOPTS": [
        # Object file doesn't export any previously undefined symbols
        "-ignore:4221",
    ],
    # "HWAES" is an abbreviation for "hardware AES" (AES - Advanced Encryption
    # Standard). These flags are used for detecting whether or not the target
    # architecture has hardware support for AES instructions which can be used
    # to improve performance of some random bit generators.
    "ABSL_RANDOM_HWAES_ARM64_FLAGS": ["-march=armv8-a+crypto"],
    "ABSL_RANDOM_HWAES_ARM32_FLAGS": ["-mfpu=neon"],
    "ABSL_RANDOM_HWAES_X64_FLAGS": [
        "-maes",
        "-msse4.1",
    ],
    "ABSL_RANDOM_HWAES_MSVC_X64_FLAGS": [],
}
