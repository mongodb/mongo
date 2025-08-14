"""Custom configuration transitions."""

def _extensions_transition_impl(settings, attr):
    """
    Transition that enables bazel flags necessary to build extensions successfully.
    """
    return {
        "//bazel/config:allocator": "system",
        "//bazel/config:shared_archive": True,
        "//bazel/config:linkstatic": True,
        "//bazel/config:skip_archive": True,
        "//bazel/config:asan": settings.get("//bazel/config:asan", False),
        "//bazel/config:fsan": settings.get("//bazel/config:fsan", False),
        "//bazel/config:lsan": settings.get("//bazel/config:lsan", False),
        "//bazel/config:msan": settings.get("//bazel/config:msan", False),
        "//bazel/config:tsan": settings.get("//bazel/config:tsan", False),
        "//bazel/config:ubsan": settings.get("//bazel/config:ubsan", False),
    }

extensions_transition = transition(
    implementation = _extensions_transition_impl,
    inputs = [],
    outputs = [
        "//bazel/config:allocator",
        "//bazel/config:shared_archive",
        "//bazel/config:linkstatic",
        "//bazel/config:skip_archive",
        "//bazel/config:asan",
        "//bazel/config:fsan",
        "//bazel/config:lsan",
        "//bazel/config:msan",
        "//bazel/config:tsan",
        "//bazel/config:ubsan",
    ],
)
