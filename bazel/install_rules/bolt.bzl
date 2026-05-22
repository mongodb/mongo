SKIP_FUNCTIONS = []

def _bolt_instrument_impl(ctx):
    input_binary = ctx.files.binary_to_instrument[0]
    output_binary = ctx.actions.declare_file(ctx.files.binary_to_instrument[0].basename)
    functions_to_skip = ",".join(SKIP_FUNCTIONS)
    ctx.actions.run(
        inputs = [input_binary, ctx.files._bolt_needed_lib[0]],
        outputs = [output_binary],
        executable = ctx.executable._bolt_binary,
        arguments = [
            input_binary.path,
            "-instrument",
            "-o",
            output_binary.path,
            "--instrumentation-file=" + ctx.attr.instrumentation_output_file,
            # Flush profile to disk every N seconds. The runtime's exit-time dump
            # is registered via DT_FINI, which _exit() (used by mongod's quick
            # exit path) bypasses -- so without this we'd never get a dump. Using
            # exit() instead would trigger global destructors that hang shutdown.
            "--instrumentation-sleep-time=30",
            # Without this, the runtime resets counters after each periodic dump
            # and each .fdata file only reflects the last interval. We want the
            # file to be a cumulative snapshot so an abrupt shutdown still yields
            # a full-run profile.
            "--instrumentation-no-counters-clear",
            "--skip-funcs=" + functions_to_skip,
        ],
        mnemonic = "BoltInstrument",
    )
    return DefaultInfo(files = depset([output_binary]))

bolt_instrument = rule(
    implementation = _bolt_instrument_impl,
    attrs = {
        "binary_to_instrument": attr.label(allow_files = True),
        "instrumentation_output_file": attr.string(),
        "_bolt_binary": attr.label(allow_single_file = True, default = "@bolt_binaries//:bolt", executable = True, cfg = "exec"),
        "_bolt_needed_lib": attr.label(allow_single_file = True, default = "@bolt_binaries//:libbolt_rt_instr"),
    },
)

def _bolt_optimize_impl(ctx):
    input_binary = ctx.files.binary_to_optimize[0]
    output_binary = ctx.actions.declare_file(ctx.label.name + "/" + input_binary.basename)
    functions_to_skip = ",".join(SKIP_FUNCTIONS)
    ctx.actions.run(
        inputs = [input_binary, ctx.files.perf_data[0]],
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
            # objcopy will leave bad data like program headers in the .debug file without this
            "--use-gnu-stack",
            "-skip-funcs=" + functions_to_skip,
        ],
        mnemonic = "BoltOptimize",
        execution_requirements = {
            "no-cache": "1",
            "no-sandbox": "1",
            "no-remote": "1",
            "local": "1",
        },
    )

    return [
        DefaultInfo(files = depset([output_binary]), executable = output_binary),
        ctx.attr.binary_to_optimize[CcInfo],
        ctx.attr.binary_to_optimize[DebugPackageInfo],
        RunEnvironmentInfo(
            environment = ctx.attr.binary_to_optimize[RunEnvironmentInfo].environment,
            inherited_environment = ctx.attr.binary_to_optimize[RunEnvironmentInfo].inherited_environment,
        ),
    ]

bolt_optimize = rule(
    implementation = _bolt_optimize_impl,
    attrs = {
        "binary_to_optimize": attr.label(allow_files = True, providers = [CcInfo]),
        "perf_data": attr.label(allow_single_file = True),
        "_bolt_binary": attr.label(allow_single_file = True, default = "@bolt_binaries//:bolt", executable = True, cfg = "exec"),
    },
    executable = True,
    provides = [CcInfo, DefaultInfo],
)
