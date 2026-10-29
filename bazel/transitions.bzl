"""Custom configuration transitions."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _optimize_test_binaries_transition_impl(settings, attr):
    if settings["//bazel/config:optimize_test_binaries"]:
        return {
            "//bazel/config:asan": False,
            "//bazel/config:tsan": False,
            "//bazel/config:ubsan": False,
            "//bazel/config:linkstatic": True,
            "//bazel/config:opt": "on",
        }
    return {
        "//bazel/config:asan": settings["//bazel/config:asan"],
        "//bazel/config:tsan": settings["//bazel/config:tsan"],
        "//bazel/config:ubsan": settings["//bazel/config:ubsan"],
        "//bazel/config:linkstatic": settings["//bazel/config:linkstatic"],
        "//bazel/config:opt": settings["//bazel/config:opt"],
    }

optimize_test_binaries_transition = transition(
    implementation = _optimize_test_binaries_transition_impl,
    inputs = [
        "//bazel/config:optimize_test_binaries",
        "//bazel/config:asan",
        "//bazel/config:tsan",
        "//bazel/config:ubsan",
        "//bazel/config:linkstatic",
        "//bazel/config:opt",
    ],
    outputs = [
        "//bazel/config:asan",
        "//bazel/config:tsan",
        "//bazel/config:ubsan",
        "//bazel/config:linkstatic",
        "//bazel/config:opt",
    ],
)

def _optimize_test_binary_impl(ctx):
    # Bazel 7 wraps attr.label+cfg targets in a list even for non-split transitions.
    binary = ctx.attr.binary[0]
    inner_exe = binary[DefaultInfo].files_to_run.executable

    # Executable rules must produce their own output file; create a symlink.
    exe_extension = ".exe" if ctx.target_platform_has_constraint(
        ctx.attr._windows_constraint[platform_common.ConstraintValueInfo],
    ) else ""
    out = ctx.actions.declare_file(ctx.label.name + exe_extension)
    ctx.actions.symlink(output = out, target_file = inner_exe)

    return [
        DefaultInfo(
            files = depset(
                [out] + [f for f in binary[DefaultInfo].files.to_list() if f != inner_exe],
            ),
            executable = out,
            runfiles = binary[DefaultInfo].default_runfiles,
        ),
        binary[CcInfo],
    ]

optimize_test_binary = rule(
    implementation = _optimize_test_binary_impl,
    attrs = {
        "binary": attr.label_list(
            cfg = optimize_test_binaries_transition,
        ),
        "_windows_constraint": attr.label(default = "@platforms//os:windows"),
    },
    executable = True,
)

def _extensions_transition_impl(settings, attr):
    """
    Transition that enables bazel flags necessary to build extensions successfully.
    """
    return {
        "//bazel/config:allocator": "system",
        "//bazel/config:dtlto": False,
        "//bazel/config:shared_archive": True,
        "//bazel/config:linkstatic": True,
        "//bazel/config:pgo_profile_use": False,
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
        "//bazel/config:dtlto",
        "//bazel/config:shared_archive",
        "//bazel/config:linkstatic",
        "//bazel/config:pgo_profile_use",
        "//bazel/config:skip_archive",
        "//bazel/config:asan",
        "//bazel/config:fsan",
        "//bazel/config:lsan",
        "//bazel/config:msan",
        "//bazel/config:tsan",
        "//bazel/config:ubsan",
    ],
)
