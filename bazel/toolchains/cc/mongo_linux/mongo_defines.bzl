"""This module provides a list of defines that is passed in to compiling specifically to linux builds.
"""

visibility(["//bazel/toolchains/cc/mongo_linux"])

DEFINES = [
    # On linux, C code compiled with gcc/clang -std=c11 causes
    # __STRICT_ANSI__ to be set, and that drops out all of the feature test
    # definitions, resulting in confusing errors when we run C language
    # configure checks and expect to be able to find newer POSIX things.
    # Explicitly enabling _XOPEN_SOURCE fixes that, and should be mostly
    # harmless as on Linux, these macros are cumulative. The C++ compiler
    # already sets _XOPEN_SOURCE, and, notably, setting it again does not
    # disable any other feature test macros, so this is safe to do. Other
    # platforms like macOS and BSD have crazy rules, so don't try this
    # there.
    #
    # Furthermore, as both C++ compilers appear to define _GNU_SOURCE
    # unconditionally (because libstdc++ requires it), it seems prudent to
    # explicitly add that too, so that C language checks see a consistent
    # set of definitions.
    "_XOPEN_SOURCE=700",
    "_GNU_SOURCE",
]
