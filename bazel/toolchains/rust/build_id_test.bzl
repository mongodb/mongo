"""Check that separately linked Rust binaries and debug symbols remain compatible."""

def _build_mode(settings, stripped):
    features = [feature for feature in settings["//command_line_option:features"] if feature not in ["strip_debug", "-thin_archive"]]
    return {
        "//command_line_option:features": features + (["strip_debug", "-thin_archive"] if stripped else []),
        "//bazel/config:simple_build_id": True,
        "//bazel/config:separate_debug": not stripped,
        "//bazel/config:gdb_index": False,
        "//bazel/config:remote_link": stripped,
    }

def _stripped_impl(settings, _attr):
    return _build_mode(settings, True)

def _debug_impl(settings, _attr):
    return _build_mode(settings, False)

_BUILD_MODE_OUTPUTS = [
    "//command_line_option:features",
    "//bazel/config:simple_build_id",
    "//bazel/config:separate_debug",
    "//bazel/config:gdb_index",
    "//bazel/config:remote_link",
]

_stripped = transition(
    implementation = _stripped_impl,
    inputs = ["//command_line_option:features"],
    outputs = _BUILD_MODE_OUTPUTS,
)

_debug = transition(
    implementation = _debug_impl,
    inputs = ["//command_line_option:features"],
    outputs = _BUILD_MODE_OUTPUTS,
)

def _build_id_test_impl(ctx):
    stripped_files = ctx.attr.stripped[0].files.to_list()
    debug_files = ctx.attr.debug[0].files.to_list()
    stripped_binary = [file for file in stripped_files if file.basename.endswith(".so")][0]
    debug_binary = [file for file in debug_files if file.basename.endswith(".so")][0]
    debug_symbols = [file for file in debug_files if file.basename.endswith(".so.debug")][0]

    executable = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        executable,
        """#!/usr/bin/env bash
set -euo pipefail
readelf="$PWD/{readelf}"
objcopy="$PWD/{objcopy}"
stripped="$PWD/build-id/stripped.so"
debug="$PWD/build-id/debug.so"
symbols="$PWD/build-id/debug.so.debug"

build_id() {{
    "$readelf" --notes "$1" | sed -n 's/.*Build ID: //p'
}}

stripped_id=$(build_id "$stripped")
debug_id=$(build_id "$debug")
symbols_id=$(build_id "$symbols")
# simple_build_id uses the same three 32-bit hashes as the C++ build rules.
if [[ ${{#stripped_id}} != 24 ]]; then
    echo "Expected a simple build ID, got: $stripped_id" >&2
    exit 1
fi
if [[ -z "$stripped_id" || "$stripped_id" != "$debug_id" || "$stripped_id" != "$symbols_id" ]]; then
    echo "Build IDs differ: stripped=$stripped_id debug=$debug_id symbols=$symbols_id" >&2
    exit 1
fi

# Compare loadable section contents, including unwind data, independently of debug sections.
"$objcopy" -O binary "$stripped" "$TEST_TMPDIR/stripped.bin"
"$objcopy" -O binary "$debug" "$TEST_TMPDIR/debug.bin"
cmp "$TEST_TMPDIR/stripped.bin" "$TEST_TMPDIR/debug.bin"
""".format(
            readelf = ctx.executable._readelf.short_path,
            objcopy = ctx.executable._objcopy.short_path,
        ),
        is_executable = True,
    )

    # Configured artifacts share short_path; stage each build mode separately.
    runfiles = ctx.runfiles(
        files = [ctx.executable._readelf, ctx.executable._objcopy],
        transitive_files = ctx.attr._tool_runtime_files.files,
        symlinks = {
            "build-id/stripped.so": stripped_binary,
            "build-id/debug.so": debug_binary,
            "build-id/debug.so.debug": debug_symbols,
        },
    )
    for tool in [ctx.attr._readelf, ctx.attr._objcopy]:
        runfiles = runfiles.merge(tool[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = executable, runfiles = runfiles)]

rust_build_id_test = rule(
    doc = "Verify simple build IDs and loadable bytes across the two CI link modes.",
    implementation = _build_id_test_impl,
    attrs = {
        "stripped": attr.label(mandatory = True, cfg = _stripped),
        "debug": attr.label(mandatory = True, cfg = _debug),
        "_readelf": attr.label(default = "@mongo_toolchain_v5//:llvm_readelf", cfg = "exec", executable = True),
        "_objcopy": attr.label(default = "@mongo_toolchain_v5//:llvm_objcopy", cfg = "exec", executable = True),
        "_tool_runtime_files": attr.label(default = "@mongo_toolchain_v5//:tool_runtime_files", cfg = "exec"),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
    test = True,
)
