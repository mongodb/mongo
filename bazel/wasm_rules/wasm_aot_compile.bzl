"""Rule to AOT-compile a WASM component using the wasmtime CLI by default.

The wasmtime serialized format embeds engine configuration metadata. The tool
that produces the .cwasm must be built with the same wasmtime library build
(same Bazel configuration) as the binary that will deserialize it.

Default tool is the wasmtime CLI from the wasmtime-cli crate (see MODULE.bazel).
The rule passes --target <triple> to wasmtime compile, where the triple comes
from the Rust toolchain's target_triple (the platform we are building for).
If you do not pass --target it will only work on the current machine you run it on.
"""

def _aot_compile_wasm_impl(ctx):
    tool = ctx.executable.tool
    input_file = ctx.file.src
    output_file = ctx.outputs.out

    toolchain = ctx.toolchains["@rules_rust//rust:toolchain_type"]
    triple_str = toolchain.target_triple.str

    ctx.actions.run(
        inputs = [input_file],
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
            "cache=no",
            # -W sets wasmtime runtime options.
            # Thes options must match the options we pass at
            # startup or else starting the module will throw.
            "-W",
            "epoch-interruption=y",
        ],
        mnemonic = "WasmAotCompile",
        progress_message = "AOT compiling %s" % input_file.short_path,
    )

aot_compile_wasm = rule(
    implementation = _aot_compile_wasm_impl,
    attrs = {
        "tool": attr.label(
            default = Label("@crates//:wasmtime-cli__wasmtime"),
            executable = True,
            cfg = "exec",
            doc = "Executable that performs AOT compile (default: wasmtime CLI). Built for exec platform.",
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
)
