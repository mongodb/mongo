SKIP_FUNCTIONS = []

def _bolt_instrument_impl(ctx):
    input_binary = ctx.files.binary_to_instrument[0]
    output_binary = ctx.actions.declare_file(ctx.files.binary_to_instrument[0].basename)
    functions_to_skip = ",".join(SKIP_FUNCTIONS)
    ctx.actions.run(
        inputs = [input_binary, ctx.files._bolt_needed_lib[0]],
        outputs = [output_binary],
        executable = ctx.executable._bolt_binary,
        arguments = [input_binary.path, "-instrument", "-o", output_binary.path, "--instrumentation-file=" + ctx.attr.instrumentation_output_file, "--instrumentation-file-append-pid", "--skip-funcs=" + functions_to_skip],
        mnemonic = "BoltInstrument",
    )
    return DefaultInfo(files = depset([output_binary]))

bolt_instrument = rule(
    implementation = _bolt_instrument_impl,
    attrs = {
        "binary_to_instrument": attr.label(allow_files = True),
        "instrumentation_output_file": attr.string(),
        "_bolt_binary": attr.label(allow_single_file = True, default = "@bolt_binaries//:bolt", executable = True, cfg = "host"),
        "_bolt_needed_lib": attr.label(allow_single_file = True, default = "@bolt_binaries//:libbolt_rt_instr"),
    },
)

def _bolt_optimize_impl(ctx):
    input_binary = ctx.files.binary_to_optimize[0]
    output_binary = ctx.actions.declare_file(ctx.files.binary_to_optimize[0].basename)
    functions_to_skip = ",".join(SKIP_FUNCTIONS)
    ctx.actions.run(
        inputs = [input_binary],
        outputs = [output_binary],
        executable = ctx.executable._bolt_binary,
        arguments = [
            input_binary.path,
            "-o",
            output_binary.path,
            "-data",
            ctx.files.perf_data[0].path,
            "-reorder-blocks=ext-tsp",
            "-reorder-functions=cdsort",
            "-split-functions",
            "-split-all-cold",
            "-split-eh",
            "-dyno-stats",
            "--lite",
            "--update-debug-sections",
            "-skip-funcs=" + functions_to_skip,
        ],
        mnemonic = "BoltOptimize",
    )
    return DefaultInfo(files = depset([output_binary]))

bolt_optimize = rule(
    implementation = _bolt_optimize_impl,
    attrs = {
        "binary_to_optimize": attr.label(allow_files = True),
        "perf_data": attr.label(allow_single_file = True),
        "_bolt_binary": attr.label(allow_single_file = True, default = "@bolt_binaries//:bolt", executable = True, cfg = "host"),
    },
)
