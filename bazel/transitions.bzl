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
    }

extensions_transition = transition(
    implementation = _extensions_transition_impl,
    inputs = [],
    outputs = [
        "//bazel/config:allocator",
        "//bazel/config:shared_archive",
        "//bazel/config:linkstatic",
        "//bazel/config:skip_archive",
    ],
)
