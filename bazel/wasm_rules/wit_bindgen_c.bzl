"""Rule to generate C bindings from WIT files using wit-bindgen."""

def _wit_bindgen_c_impl(ctx):
    tool = ctx.executable.tool
    wit_file = ctx.file.wit

    out_h = ctx.outputs.out_h
    out_c = ctx.outputs.out_c
    out_component_type = ctx.outputs.out_component_type

    ctx.actions.run(
        inputs = [wit_file],
        outputs = [out_h, out_c, out_component_type],
        executable = tool,
        arguments = [
            "c",
            wit_file.dirname,
            "--out-dir",
            out_h.dirname,
        ],
        mnemonic = "WitBindgenC",
        progress_message = "Generating C bindings from %s" % wit_file.short_path,
    )

wit_bindgen_c = rule(
    implementation = _wit_bindgen_c_impl,
    attrs = {
        "tool": attr.label(
            default = Label("@cargo_bindeps//:wit-bindgen-cli__wit-bindgen"),
            executable = True,
            cfg = "exec",
            doc = "wit-bindgen CLI executable.",
        ),
        "wit": attr.label(
            allow_single_file = [".wit"],
            mandatory = True,
            doc = "Input .wit file.",
        ),
        "out_h": attr.output(mandatory = True, doc = "Output C header file."),
        "out_c": attr.output(mandatory = True, doc = "Output C source file."),
        "out_component_type": attr.output(mandatory = True, doc = "Output component type .o file."),
    },
)
