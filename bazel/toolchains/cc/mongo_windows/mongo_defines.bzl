"""This module provides a list of defines that is passed in to compiling.
"""

visibility(["//bazel/toolchains/cc"])

WIN_BOOST_DEFINES = select({
    "@platforms//os:windows": ["BOOST_ALL_NO_LIB"],
    "//conditions:default": [],
})

WINDOWS_DEFAULT_DEFINES = select({
    "@platforms//os:windows": [
        # This tells the Windows compiler not to link against the .lib files and
        # to use boost as a bunch of header-only libraries.
        "BOOST_ALL_NO_LIB",
        "_UNICODE",
        "UNICODE",

        # Temporary fixes to allow compilation with VS2017.
        "_SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING",
        "_SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING",
        "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING",

        # TODO(SERVER-60151): Until we are fully in C++20 mode, it is easier to
        # simply suppress C++20 deprecations. After we have switched over we
        # should address any actual deprecated usages and then remove this flag.
        "_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS",
        "_CONSOLE",
        "_CRT_SECURE_NO_WARNINGS",
        "_ENABLE_EXTENDED_ALIGNED_STORAGE",
        "_SCL_SECURE_NO_WARNINGS",
    ],
    "//conditions:default": [],
})

WINDOWS_DEFINES = WIN_BOOST_DEFINES + WINDOWS_DEFAULT_DEFINES
