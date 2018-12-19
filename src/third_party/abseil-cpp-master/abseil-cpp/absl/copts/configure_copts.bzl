"""absl specific copts.

This file simply selects the correct options from the generated files.  To
change Abseil copts, edit absl/copts/copts.py
"""

load(
    "//absl:copts/GENERATED_copts.bzl",
    "GCC_EXCEPTIONS_FLAGS",
    "GCC_FLAGS",
    "GCC_TEST_FLAGS",
    "LLVM_EXCEPTIONS_FLAGS",
    "LLVM_FLAGS",
    "LLVM_TEST_FLAGS",
    "MSVC_EXCEPTIONS_FLAGS",
    "MSVC_FLAGS",
    "MSVC_TEST_FLAGS",
)

ABSL_DEFAULT_COPTS = select({
    "//absl:windows": MSVC_FLAGS,
    "//absl:llvm_compiler": LLVM_FLAGS,
    "//conditions:default": GCC_FLAGS,
})

# in absence of modules (--compiler=gcc or -c opt), cc_tests leak their copts
# to their (included header) dependencies and fail to build outside absl
ABSL_TEST_COPTS = ABSL_DEFAULT_COPTS + select({
    "//absl:windows": MSVC_TEST_FLAGS,
    "//absl:llvm_compiler": LLVM_TEST_FLAGS,
    "//conditions:default": GCC_TEST_FLAGS,
})

ABSL_EXCEPTIONS_FLAG = select({
    "//absl:windows": MSVC_EXCEPTIONS_FLAGS,
    "//absl:llvm_compiler": LLVM_EXCEPTIONS_FLAGS,
    "//conditions:default": GCC_EXCEPTIONS_FLAGS,
})

ABSL_EXCEPTIONS_FLAG_LINKOPTS = select({
    "//conditions:default": [],
})
