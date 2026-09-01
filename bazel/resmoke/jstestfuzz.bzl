"""Rule for generating randomized jstests via jstestfuzz (see jstestfuzz_generate)."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

# Auto-sharding target: roughly this many generated files per shard/action.
_FILES_PER_SHARD = 15

def _jstestfuzz_generate_impl(ctx):
    # Per-target seed overrides the global flag.
    seed = ctx.attr.seed if ctx.attr.seed else ctx.attr._seed_flag[BuildSettingInfo].value
    branch = ctx.attr._branch_flag[BuildSettingInfo].value

    num_files = ctx.attr.num_generated_files
    if num_files <= 0:
        fail("num_generated_files must be > 0, got %d" % num_files)

    shards = ctx.attr.shard_count
    if shards <= 0:
        shards = max(1, (num_files + _FILES_PER_SHARD - 1) // _FILES_PER_SHARD)
    shards = min(shards, num_files)

    execution_requirements = {
        # Output depends on the seed, which is volatile (random per build) unless
        # a fixed seed is pinned; never cache when it isn't.
        "no-cache": "1",
    } if not seed else {}

    npm_cli = ctx.file._npm_cli.path
    bundle = ctx.file.bundle
    tool_inputs = [bundle] + ctx.files._node_files

    template_files = ctx.files.js_tests
    js_tests_dir = None
    if ctx.attr.js_tests:
        pkg = ctx.attr.js_tests.label.package
        js_tests_dir = ctx.bin_dir.path + "/" + pkg if pkg else ctx.bin_dir.path

    # Fan generation out across independent actions so Bazel runs them in
    # parallel. Each shard gets its own output directory and a distinct seed
    # (base seed + shard index, derived in the wrapper), which also guarantees
    # unique generated filenames since jstestfuzz embeds the seed in each name.
    out_dirs = []
    for i in range(shards):
        dir_name = ctx.label.name + "_out" if shards == 1 else "%s_out_%d" % (ctx.label.name, i)
        out_dir = ctx.actions.declare_directory(dir_name)
        out_dirs.append(out_dir)
        shard_files = num_files // shards + (1 if i < num_files % shards else 0)

        args = ctx.actions.args()
        args.add("--out-dir", out_dir.path)
        args.add("--bundle", bundle.path)
        args.add("--npm-cli", npm_cli)
        args.add("--npm-command", ctx.attr.npm_command)
        args.add("--num-generated-files", str(shard_files))
        args.add("--branch", branch)
        args.add("--seed-offset", str(i))
        if js_tests_dir:
            args.add("--js-tests-dir", js_tests_dir)

        # TODO(DEVPROD-10137): Remove this conditional logic once `--useEsModules`
        # is a top-level supported flag of jstestfuzz.
        if ctx.attr.npm_command != "jstestfuzz":
            args.add("--use-es-modules")

        inputs = tool_inputs + template_files
        if seed:
            args.add("--seed", seed)
        else:
            # No fixed seed: derive from volatile-status so each run gets fresh tests.
            args.add("--volatile-status", ctx.version_file.path)
            inputs = inputs + [ctx.version_file]

        args.add("--")
        args.add_all(ctx.attr.extra_args)

        ctx.actions.run(
            executable = ctx.executable._wrapper,
            inputs = inputs,
            outputs = [out_dir],
            arguments = [args],
            mnemonic = "JSTestFuzz",
            progress_message = "jstestfuzz: %s n=%d shard %d/%d %s" % (
                ctx.attr.npm_command,
                shard_files,
                i + 1,
                shards,
                "(seed=" + seed + ")" if seed else "(random seed)",
            ),
            execution_requirements = execution_requirements,
        )

    return [DefaultInfo(files = depset(out_dirs))]

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
        "shard_count": attr.int(
            default = 0,
            doc = "Number of parallel generation actions to fan out to.  Each " +
                  "shard produces ~num_generated_files/shard_count tests into " +
                  "its own output directory using seed base_seed+shard_index. " +
                  "0 (the default) auto-shards at ~%d files per shard." % _FILES_PER_SHARD,
        ),
        "extra_args": attr.string_list(
            default = [],
            doc = "Additional CLI flags forwarded verbatim to jstestfuzz, " +
                  "suite-specific flags like ['--opType', 'moveCollection']. " +
                  "Do not pass --jsTestsDir here; use the js_tests attr.",
        ),
        "js_tests": attr.label(
            default = None,
            doc = "The template corpus passed to jstestfuzz as --jsTestsDir. " +
                  "Defaults to None (no corpus) since most npm commands (e.g. " +
                  "resharding-fuzzer) do not accept --jsTestsDir. Set to " +
                  "//jstests:all_subpackage_javascript_files (or a subtree) for " +
                  "commands that mutate an existing corpus (e.g. jstestfuzz, agg-fuzzer).",
        ),
        "_npm_cli": attr.label(
            default = "//bazel/resmoke:jstestfuzz_npm_cli",
            allow_single_file = True,
            cfg = "exec",
            doc = "npm-cli.js, invoked directly with node (avoids the bin/npm " +
                  "symlink); the wrapper locates node relative to it.",
        ),
        "_node_files": attr.label(
            default = "//bazel/resmoke:jstestfuzz_node_files",
            cfg = "exec",
            doc = "The full node distribution, shipped as inputs.",
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
        "bundle": attr.label(
            default = "@jstestfuzz//:bundle",
            allow_single_file = True,
            doc = "Tarball of the prepared jstestfuzz checkout (sources + " +
                  "node_modules + compiled grammars), unpacked per action.",
        ),
    },
    doc = "Generates randomized .js test files with jstestfuzz.",
)
