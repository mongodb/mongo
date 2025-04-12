"""This file contains compiler flags that is specific to Mac C++ compiling and linking."""

# Flags listed in this file is only visible to the bazel build system.
visibility("//bazel/toolchains/cc")

DEDUPE_SYMBOL_LINKFLAGS = select({
    "//bazel/config:macos_opt_off": ["-Wl,-no_deduplicate"],
    "//conditions:default": [],
})

MONGO_MAC_CC_LINKFLAGS = (
    DEDUPE_SYMBOL_LINKFLAGS
)
