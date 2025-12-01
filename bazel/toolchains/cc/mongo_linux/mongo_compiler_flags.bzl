"""
DO NOT ADD TO THIS FILE. FLAGS SHOULD BE ADDED THROUGH THE TOOLCHAIN INSTEAD.
This file contains compiler flags that are specific to linux cc compiling and linking.
"""

load(
    "//bazel/toolchains/cc:mongo_errors.bzl",
    "BAZELISK_CHECK_ERROR_MESSAGE",
)

# Flags listed in this file is only visible to the bazel build system.
visibility("//bazel/toolchains/cc")

# TODO(SERVER-101099): Remove this once builds are containerized and system libraries inside the containers
# no longer contain debug symbols.
#
# In RHEL8 and RHEL9 the debug symbols for libgcc aren't stripped and are instead split, which still leaves behind
# debug symbols in the libgcc shared object file. These debug symbols are created with gdwarf32, so they're limited to
# a 32 bit address space. Even if the mongodb binaries are compiled with gdwarf64, there's a chance that the gdwarf32
# libgcc debug symbols will be placed after the gdwarf64 debug symbols. This started happening in the RHEL9 ppc64le
# build.
#
# The workaround for this is stripping the debug symbols from libgcc and statically compiling the libgcc from the
# toolchain into the mongodb binaries. The longer term solution for this is to containerize the non-remote-execution
# build and strip the debug symbols inside the container, or patch the compilers to properly order gdwarf32 symbols
# before gdwarf64 symbols. See https://reviews.llvm.org/D96144
LIBGCC_LINKFLAGS = select({
    "//bazel/config:rhel9_ppc64le_gcc_linkstatic": ["-static-libgcc"],
    "//conditions:default": [],
})

# TODO(SERVER-113302): Deprecate after 8.3 release due to using PGO instead
SYMBOL_ORDER_COPTS = select({
    "//bazel/config:symbol_ordering_file_enabled": ["-ffunction-sections"],
    "//bazel/config:symbol_ordering_file_enabled_al2023": ["-ffunction-sections"],
    "//conditions:default": [],
})

# TODO(SERVER-113302): Deprecate after 8.3 release due to using PGO instead
SYMBOL_ORDER_LINKFLAGS = select({
    "//bazel/config:symbol_ordering_file_enabled": [
        "-Wl,--symbol-ordering-file=$(location //buildscripts:symbols.orderfile)",
        "-Wl,--no-warn-symbol-ordering",
    ],
    "//bazel/config:symbol_ordering_file_enabled_al2023": [
        "-Wl,--symbol-ordering-file=$(location //buildscripts:symbols-al2023.orderfile)",
        "-Wl,--no-warn-symbol-ordering",
    ],
    "//conditions:default": [],
})

# Hack to throw an error if the user isn't running bazel through bazelisk,
# since we want to make sure the hook inside of tools/bazel gets run.
RUNNING_THROUGH_BAZELISK_CHECK = select({
    "//bazel/config:running_through_bazelisk_x86_64_or_arm64": [],
    "@platforms//cpu:s390x": [],
    "@platforms//cpu:ppc": [],
}, no_match_error = BAZELISK_CHECK_ERROR_MESSAGE)

MONGO_GLOBAL_INCLUDE_DIRECTORIES = [
    "-Isrc",
    "-I$(GENDIR)/src",
    "-I$(GENDIR)/src/mongo/db/modules/enterprise/src",
    "-Isrc/mongo/db/modules/enterprise/src",
    "-Isrc/third_party/valgrind/include",
]

MONGO_LINUX_CC_COPTS = (
    MONGO_GLOBAL_INCLUDE_DIRECTORIES +
    SYMBOL_ORDER_COPTS +
    RUNNING_THROUGH_BAZELISK_CHECK
)

MONGO_LINUX_CC_LINKFLAGS = (
    SYMBOL_ORDER_LINKFLAGS +
    LIBGCC_LINKFLAGS
)
