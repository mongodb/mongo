"""Abseil compiler options.

This is the source of truth for Abseil compiler options.  To modify Abseil
compilation options:

  (1) Edit the appropriate list in this file based on the platform the flag is
      needed on.
  (2) Run `<path_to_absl>/copts/generate_copts.py`.

The generated copts are consumed by configure_copts.bzl and
AbseilConfigureCopts.cmake.
"""

import collections  # absl:google-only(used for internal flags)
COPT_VARS = {
    "GCC_FLAGS": [
        "-Wall",
        "-Wextra",
        "-Wcast-qual",
        "-Wconversion-null",
        "-Wmissing-declarations",
        "-Woverlength-strings",
        "-Wpointer-arith",
        "-Wunused-local-typedefs",
        "-Wunused-result",
        "-Wvarargs",
        "-Wvla",  # variable-length array
        "-Wwrite-strings",
        # Google style does not use unsigned integers, though STL containers
        # have unsigned types.
        "-Wno-sign-compare",
    ],
    "GCC_TEST_FLAGS": [
        "-Wno-conversion-null",
        "-Wno-missing-declarations",
        "-Wno-sign-compare",
        "-Wno-unused-function",
        "-Wno-unused-parameter",
        "-Wno-unused-private-field",
    ],
    "GCC_EXCEPTIONS_FLAGS": ["-fexceptions"],

    # Docs on single flags is preceded by a comment.
    # Docs on groups of flags is preceded by ###.
    "LLVM_FLAGS": [
        "-Wall",
        "-Wextra",
        "-Weverything",
        # Abseil does not support C++98
        "-Wno-c++98-compat-pedantic",
        # Turns off all implicit conversion warnings. Most are re-enabled below.
        "-Wno-conversion",
        "-Wno-covered-switch-default",
        "-Wno-deprecated",
        "-Wno-disabled-macro-expansion",
        "-Wno-double-promotion",
        ###
        # Turned off as they include valid C++ code.
        "-Wno-comma",
        "-Wno-extra-semi",
        "-Wno-extra-semi-stmt",
        "-Wno-packed",
        "-Wno-padded",
        ###
        # Google style does not use unsigned integers, though STL containers
        # have unsigned types.
        "-Wno-sign-compare",
        ###
        "-Wno-float-conversion",
        "-Wno-float-equal",
        "-Wno-format-nonliteral",
        # Too aggressive: warns on Clang extensions enclosed in Clang-only
        # compilation paths.
        "-Wno-gcc-compat",
        ###
        # Some internal globals are necessary. Don't do this at home.
        "-Wno-global-constructors",
        "-Wno-exit-time-destructors",
        ###
        "-Wno-nested-anon-types",
        "-Wno-non-modular-include-in-module",
        "-Wno-old-style-cast",
        # Warns on preferred usage of non-POD types such as string_view
        "-Wno-range-loop-analysis",
        "-Wno-reserved-id-macro",
        "-Wno-shorten-64-to-32",
        "-Wno-switch-enum",
        "-Wno-thread-safety-negative",
        "-Wno-undef",
        "-Wno-unknown-warning-option",
        "-Wno-unreachable-code",
        # Causes warnings on include guards
        "-Wno-unused-macros",
        "-Wno-weak-vtables",
        ###
        # Implicit conversion warnings turned off by -Wno-conversion
        # which are re-enabled below.
        "-Wbitfield-enum-conversion",
        "-Wbool-conversion",
        "-Wconstant-conversion",
        "-Wenum-conversion",
        "-Wint-conversion",
        "-Wliteral-conversion",
        "-Wnon-literal-null-conversion",
        "-Wnull-conversion",
        "-Wobjc-literal-conversion",
        "-Wno-sign-conversion",
        "-Wstring-conversion",
    ],
    "LLVM_TEST_FLAGS": [
        "-Wno-c99-extensions",
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
    ],
    "LLVM_EXCEPTIONS_FLAGS": ["-fexceptions"],
    # /Wall with msvc includes unhelpful warnings such as C4711, C4710, ...
    "MSVC_FLAGS": [
        "/W3",
        "/wd4005",  # macro-redefinition
        "/wd4068",  # unknown pragma
        "/wd4180",  # qualifier applied to function type has no meaning; ignored
        "/wd4244",  # conversion from 'type1' to 'type2', possible loss of data
        "/wd4267",  # conversion from 'size_t' to 'type', possible loss of data
        # forcing value to bool 'true' or 'false' (performance warning)
        "/wd4800",
        "/DNOMINMAX",  # Don't define min and max macros (windows.h)
        # Don't bloat namespace with incompatible winsock versions.
        "/DWIN32_LEAN_AND_MEAN",
        # Don't warn about usage of insecure C functions.
        "/D_CRT_SECURE_NO_WARNINGS",
        "/D_SCL_SECURE_NO_WARNINGS",
        # Introduced in VS 2017 15.8, allow overaligned types in aligned_storage
        "/D_ENABLE_EXTENDED_ALIGNED_STORAGE",
    ],
    "MSVC_TEST_FLAGS": [
        "/wd4018",  # signed/unsigned mismatch
        "/wd4101",  # unreferenced local variable
        "/wd4503",  # decorated name length exceeded, name was truncated
    ],
    "MSVC_EXCEPTIONS_FLAGS": [
        "/U_HAS_EXCEPTIONS", "/D_HAS_EXCEPTIONS=1", "/EHsc"
    ]
}
