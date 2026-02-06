def _impl(settings, attr):
    _ignore = (settings, attr)
    return {
        "//command_line_option:extra_toolchains": "//bazel/toolchains/cc/mongo_wasm/toolchain:wasi_cc_toolchain_wasip2",
        "//command_line_option:platforms": "//bazel/platforms:wasm32",
        "//bazel/config:libunwind": "off",
        "//bazel/config:allocator": "system",
        "//bazel/config:linkstatic": True,
        "//bazel/config:asan": False,
        "//bazel/config:fsan": False,
        "//bazel/config:lsan": False,
        "//bazel/config:msan": False,
        "//bazel/config:tsan": False,
        "//bazel/config:ubsan": False,
    }

wasi_transition = transition(
    implementation = _impl,
    inputs = [],
    outputs = [
        "//command_line_option:extra_toolchains",
        "//command_line_option:platforms",
        "//bazel/config:libunwind",
        "//bazel/config:allocator",
        "//bazel/config:linkstatic",
        "//bazel/config:asan",
        "//bazel/config:fsan",
        "//bazel/config:lsan",
        "//bazel/config:msan",
        "//bazel/config:tsan",
        "//bazel/config:ubsan",
    ],
)
