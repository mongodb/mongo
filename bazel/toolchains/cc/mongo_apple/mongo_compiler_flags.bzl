"""This file contains compiler flags that is specific to Mac C++ compiling and linking."""

# Flags listed in this file is only visible to the bazel build system.
visibility("//bazel/toolchains/cc")

MACOS_WARNINGS_COPTS = select({
    "@platforms//os:macos": [
        # As of XCode 9, this flag must be present (it is not enabled by -Wall),
        # in order to enforce that -mXXX-version-min=YYY will enforce that you
        # don't use APIs from ZZZ.
        "-Wunguarded-availability",
        "-Wno-enum-constexpr-conversion",
    ],
    "//conditions:default": [],
})

# Enable sized deallocation support.
#
# Bazel doesn't allow for defining C++-only flags without a custom toolchain
# config. This is setup in the Linux toolchain, but currently there is no custom
# MacOS toolchain. Enabling warnings-as-errors will fail the build if this flag
# is passed to the compiler when building C code. Define it here on MacOS only
# to allow us to configure warnings-as-errors on Linux.
#
# TODO(SERVER-90183): Remove this once custom toolchain configuration is
#                     implemented on MacOS.
FSIZED_DEALLOCATION_COPT = select({
    "@platforms//os:macos": ["-fsized-deallocation"],
    "//conditions:default": [],
})

DEDUPE_SYMBOL_LINKFLAGS = select({
    "//bazel/config:macos_opt_off": ["-Wl,-no_deduplicate"],
    "//conditions:default": [],
})

MACOS_EXTRA_GLOBAL_LIBS_LINKFLAGS = select({
    "@platforms//os:macos": [
        "-lresolv",
    ],
    "//conditions:default": [],
})

MONGO_MAC_CC_COPTS = (
    MACOS_WARNINGS_COPTS +
    FSIZED_DEALLOCATION_COPT
)

MONGO_MAC_CC_LINKFLAGS = (
    MACOS_EXTRA_GLOBAL_LIBS_LINKFLAGS +
    DEDUPE_SYMBOL_LINKFLAGS
)
