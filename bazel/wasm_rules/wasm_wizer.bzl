"""Rule to pre-initialize a WASM component with Wizer.

The tool instantiates the component, runs its initialization, and snapshots the resulting state
into a new component whose initial memory is the initialized state. Instances created from it
start already initialized, and with copy-on-write memory images they share the snapshot's pages
until they write to them.
"""

def _wasm_wizer_impl(ctx):
    tool = ctx.executable.tool
    input_file = ctx.file.src
    output_file = ctx.outputs.out

    ctx.actions.run(
        inputs = depset([input_file], transitive = [ctx.attr.tool[DefaultInfo].default_runfiles.files]),
        outputs = [output_file],
        executable = tool,
        arguments = [input_file.path, output_file.path],
        mnemonic = "WasmWizer",
        progress_message = "Pre-initializing %s with Wizer" % input_file.short_path,
    )
    return [DefaultInfo(files = depset([output_file]))]

_wasm_wizer_rule = rule(
    implementation = _wasm_wizer_impl,
    attrs = {
        "tool": attr.label(
            executable = True,
            cfg = "exec",
            doc = "Wizer driver taking <input> <output>, built for the execution platform.",
        ),
        "src": attr.label(
            allow_single_file = True,
            mandatory = True,
            doc = "Input .wasm component to pre-initialize.",
        ),
        "out": attr.output(
            mandatory = True,
            doc = "Output pre-initialized .wasm component.",
        ),
    },
)

def wasm_wizer(name, tool = "//bazel/wasm_rules/wizer_driver", **kwargs):
    """Pre-initialize a WASM component with Wizer."""
    _wasm_wizer_rule(
        name = name,
        tool = tool,
        **kwargs
    )
