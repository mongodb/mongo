"""This module provides a list of defines that is passed in to compiling.
"""

visibility(["//bazel/toolchains/cc/mongo_apple"])

DEFINES = [
    # TODO SERVER-54659 - ASIO depends on std::result_of which was removed
    # in C++ 20. xcode15 does not have backwards compatibility.
    "ASIO_HAS_STD_INVOKE_RESULT",
]
