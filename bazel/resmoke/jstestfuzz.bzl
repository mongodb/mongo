load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _jstestfuzz_generate_impl(ctx):
    out_dir = ctx.actions.declare_directory(ctx.label.name + "_out")

    # Per-target seed overrides the global flag.
    seed = ctx.attr.seed if ctx.attr.seed else ctx.attr._seed_flag[BuildSettingInfo].value
    branch = ctx.attr._branch_flag[BuildSettingInfo].value

    args = ctx.actions.args()
    args.add("--out-dir", out_dir.path)
    args.add("--jstestfuzz-root", ctx.file._jstestfuzz_marker.dirname)
    args.add("--npm-command", ctx.attr.npm_command)
    args.add("--num-generated-files", str(ctx.attr.num_generated_files))
    args.add("--branch", branch)
    if ctx.attr.use_es_modules:
        args.add("--use-es-modules")

    inputs = [ctx.file._jstestfuzz_marker] + ctx.files._jstestfuzz_sources
    if seed:
        args.add("--seed", seed)
    else:
        # No fixed seed: derive from volatile-status so each run gets fresh tests.
        args.add("--volatile-status", ctx.version_file.path)
        inputs = inputs + [ctx.version_file]

    args.add("--")
    args.add_all(ctx.attr.extra_args)

    execution_requirements = {
        # The action reads jstests/ templates from the workspace, outside the
        # action's declared inputs (matching the existing evergreen flow);
        "no-sandbox": "1",
        "no-remote": "1",
        "no-cache": "1",
    }

    ctx.actions.run(
        executable = ctx.executable._wrapper,
        inputs = inputs,
        outputs = [out_dir],
        arguments = [args],
        mnemonic = "JSTestFuzz",
        progress_message = "jstestfuzz: %s n=%d %s" % (
            ctx.attr.npm_command,
            ctx.attr.num_generated_files,
            "(seed=" + seed + ")" if seed else "(random seed)",
        ),
        execution_requirements = execution_requirements,
        # The upstream npm_run.sh / npm_run.py rely on PATH (to locate node,
        # python, etc.) and on user-installed node_modules.  We're already
        # non-hermetic via no-sandbox/no-remote, so let the action see the host env.
        use_default_shell_env = True,
    )

    return [DefaultInfo(files = depset([out_dir]))]

jstestfuzz_generate = rule(
    implementation = _jstestfuzz_generate_impl,
    attrs = {
        "seed": attr.string(
            default = "",
            doc = "Fixed seed for jstestfuzz.  Overrides --//bazel/resmoke:jstestfuzz_seed " +
                  "when set.  Leave empty to use the flag (random per build by default).",
        ),
        "npm_command": attr.string(
            default = "jstestfuzz",
            doc = "The npm script in jstestfuzz's package.json to run " +
                  "(jstestfuzz, agg-fuzzer, query-fuzzer, update-fuzzer, " +
                  "rollback-fuzzer, etc.).",
        ),
        "num_generated_files": attr.int(
            mandatory = True,
            doc = "How many .js test files to emit.  Passed to jstestfuzz as " +
                  "--numGeneratedFiles.",
        ),
        "use_es_modules": attr.bool(
            default = False,
            doc = "Pass --useEsModules to jstestfuzz.  Required for npm " +
                  "commands other than the default 'jstestfuzz'.",
        ),
        "extra_args": attr.string_list(
            default = [],
            doc = "Additional CLI flags forwarded verbatim to jstestfuzz, " +
                  "e.g. ['--jsTestsDir', '../jstests'] or suite-specific " +
                  "flags like ['--opType', 'moveCollection'].",
        ),
        "_seed_flag": attr.label(
            default = "//bazel/resmoke:jstestfuzz_seed",
            providers = [BuildSettingInfo],
        ),
        "_branch_flag": attr.label(
            default = "//bazel/resmoke:jstestfuzz_branch",
            providers = [BuildSettingInfo],
        ),
        "_wrapper": attr.label(
            default = "//bazel/resmoke:jstestfuzz_run",
            executable = True,
            cfg = "exec",
        ),
        "_jstestfuzz_marker": attr.label(
            default = "@jstestfuzz//:BUILD.bazel",
            allow_single_file = True,
            doc = "Marker file used to locate the jstestfuzz checkout root.",
        ),
        "_jstestfuzz_sources": attr.label(
            default = "@jstestfuzz//:sources",
        ),
    },
    doc = "Generates a directory of randomized .js test files via jstestfuzz.",
)
