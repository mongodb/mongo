"""Custom configuration transitions."""

def _extensions_transition_impl(settings, attr):
    """
    Transition that enables system allocator and shared archive options, both necessary to build extensions.
    """
    return {
        "//bazel/config:allocator": "system",
        "//bazel/config:shared_archive": True,
    }

extensions_transition = transition(
    implementation = _extensions_transition_impl,
    inputs = [],
    outputs = [
        "//bazel/config:allocator",
        "//bazel/config:shared_archive",
    ],
)
