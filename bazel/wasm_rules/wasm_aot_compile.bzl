"""Rule to AOT-compile a WASM component using the wasmtime CLI.

The wasmtime serialized format embeds engine configuration metadata. The tool
that produces the .cwasm must be built with the same wasmtime library build
(same Bazel configuration) as the binary that will deserialize it.

Default tool is the wasmtime CLI from the wasmtime-cli crate (see MODULE.bazel).
The rule passes --target <triple> to wasmtime compile, where the triple comes
from the Rust toolchain's target_triple (the platform we are building for).
If you do not pass --target it will only work on the current machine you run it on.
"""

load("@internal_platforms_do_not_use//host:constraints.bzl", "HOST_CONSTRAINTS")

def _native_tool_transition_impl(_settings, _attr):
    # Rustc must not invoke the IBM linker on an x86/ARM worker. Split the
    # target-built CLI into remote Rust compilation and a native CppLink action.
    return {"@rules_rust//rust/settings:experimental_use_cc_common_link": True}

_native_tool_transition = transition(
    implementation = _native_tool_transition_impl,
    inputs = [],
    outputs = ["@rules_rust//rust/settings:experimental_use_cc_common_link"],
)

def _aot_compile_wasm_impl(ctx):
    native_tool = ctx.executable.native_tool
    tool = native_tool or ctx.executable.tool
    input_file = ctx.file.src
    output_file = ctx.outputs.out

    toolchain = ctx.toolchains["@rules_rust//rust:toolchain_type"]
    triple_str = toolchain.target_triple.str

    tool_target = ctx.attr.native_tool[0] if native_tool else ctx.attr.tool
    tool_inputs = tool_target[DefaultInfo].default_runfiles.files

    ctx.actions.run(
        inputs = depset([input_file], transitive = [tool_inputs]),
        outputs = [output_file],
        executable = tool,
        arguments = [
            "compile",
            "--target",
            triple_str,
            input_file.path,
            "-o",
            output_file.path,
            "-C",
            "cache=no,cranelift-opt-level=speed",
            # -W sets wasmtime runtime options.
            # These options must match the options we pass at
            # startup or else starting the module will throw.
            "-W",
            "epoch-interruption=y,exceptions=y",
        ],
        # The target-built CLI is executable on the native s390x host, not on
        # its x86/ARM RBE workers. The wrapper routes this action into the
        # native persistent container.
        execution_requirements = {"no-remote": "1"} if native_tool else {},
        exec_group = "native" if native_tool else None,
        mnemonic = "WasmAotCompile",
        progress_message = "AOT compiling %s" % input_file.short_path,
    )

_aot_compile_wasm_rule = rule(
    implementation = _aot_compile_wasm_impl,
    attrs = {
        "tool": attr.label(
            executable = True,
            cfg = "exec",
            doc = "Executable that performs AOT compile (default: wasmtime CLI). Built for exec platform.",
        ),
        "native_tool": attr.label(
            executable = True,
            cfg = _native_tool_transition,
            doc = "Target-built Wasmtime for native s390x AOT execution during cross builds.",
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
        "src": attr.label(
            allow_single_file = True,
            mandatory = True,
            doc = "Input .wasm file.",
        ),
        "out": attr.output(
            mandatory = True,
            doc = "Output .cwasm file (compiled for the target platform).",
        ),
    },
    toolchains = ["@rules_rust//rust:toolchain_type"],
    exec_groups = {"native": exec_group(exec_compatible_with = HOST_CONSTRAINTS)},
)

def aot_compile_wasm(name, tool = "@crates//:wasmtime-cli__wasmtime", **kwargs):
    """AOT compile with a native s390x CLI when cross-compiling on an IBM host."""
    _aot_compile_wasm_rule(
        name = name,
        tool = select({
            "//bazel/config:linux_s390x_cross": None,
            "//conditions:default": tool,
        }),
        native_tool = select({
            "//bazel/config:linux_s390x_cross": tool,
            "//conditions:default": None,
        }),
        **kwargs
    )
