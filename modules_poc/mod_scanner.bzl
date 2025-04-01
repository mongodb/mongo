# Based heavily on https://github.com/mongodb-forks/bazel_clang_tidy/blob/master/clang_tidy/clang_tidy.bzl
# mixed with skylib's run_binary.bzl, and then enhanced to use unused_inputs_list.
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")

def _run_mod_scan(
        ctx,  # type: ctx
        flags,
        compilation_context,  # type: CompilationContext
        infile,
        discriminator):
    cc_toolchain = find_cpp_toolchain(ctx)
    tool_inputs, tool_input_mfs = ctx.resolve_tools(tools = [ctx.attr._mod_scanner])
    inputs = depset(
        direct = (
            [infile]
        ),
        transitive = [compilation_context.headers, cc_toolchain.all_files],
    )

    # specify the output file - twice
    outfile = ctx.actions.declare_file(
        "mod_scanner/" + infile.path + "." + discriminator + ".mod_scanner_decls.json",
    )
    unused_inputs = ctx.actions.declare_file(
        "mod_scanner/" + infile.path + "." + discriminator + ".mod_scanner.unused_inputs",
    )

    # start args passed to the compiler
    args = ctx.actions.args()
    args.add(infile.path)

    # add includes
    args.add_all(compilation_context.framework_includes, before_each = "-F")
    args.add_all(compilation_context.includes, before_each = "-I")
    args.add_all(compilation_context.quote_includes, before_each = "-iquote")
    args.add_all(compilation_context.system_includes, before_each = "-isystem")

    # add args specified by the toolchain, on the command line and rule copts
    args.add_all(flags)

    # add defines
    args.add_all(compilation_context.defines, before_each = "-D")
    args.add_all(compilation_context.local_defines, before_each = "-D")

    ctx.actions.run(
        inputs = inputs,
        outputs = [outfile, unused_inputs],
        executable = ctx.executable._mod_scanner,
        arguments = [args],
        mnemonic = "ModScanner",
        #use_default_shell_env = True,
        env = {
            "MOD_SCANNER_OUTPUT": outfile.path,
            "MOD_SCANNER_UNUSED": unused_inputs.path,
        },
        progress_message = "Run mod_scanner on {}".format(infile.short_path),
        tools = tool_inputs,
        input_manifests = tool_input_mfs,
        unused_inputs_list = unused_inputs,
    )
    return outfile

def _rule_sources(ctx):
    def check_valid_file_type(src):
        """
        Returns True if the file type matches one of the permitted srcs file types for C and C++ header/source files.
        """
        permitted_file_types = [
            ".c",
            ".cc",
            ".cpp",
            ".cxx",
            ".c++",
            ".C",
            # We only analyze cc files (headers are effectively analyzed by being #include-d)
            # ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".H",
        ]
        for file_type in permitted_file_types:
            if src.basename.endswith(file_type):
                return True
        return False

    srcs = []
    if hasattr(ctx.rule.attr, "srcs"):
        for src in ctx.rule.attr.srcs:
            srcs += [src for src in src.files.to_list() if check_valid_file_type(src)]

    # Filter sources down to only those that are Mongo-specific.
    # Although we also apply a filter mechanism in the clang-tidy config itself, this filter mechanism
    # ensures we don't run clang-tidy at *all* on #include-d headers. Without this filter, Bazel
    # runs clang-tidy individual on each 3P header, which massively increases execution time.
    # For a long-term fix, see https://github.com/erenon/bazel_clang_tidy/issues/64
    return [src for src in srcs if "src/mongo/" in src.path and "third_party" not in src.path]

def _toolchain_flags(ctx, action_name = ACTION_NAMES.cpp_compile):
    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
    )
    compile_variables = cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        user_compile_flags = ctx.fragments.cpp.cxxopts + ctx.fragments.cpp.copts,
    )
    flags = cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = action_name,
        variables = compile_variables,
    )
    return flags

def _safe_flags(flags):
    # Some flags might be used by GCC, but not understood by Clang.
    # Remove them here, to allow users to run clang-tidy, without having
    # a clang toolchain configured (that would produce a good command line with --compiler clang)
    unsupported_flags = [
        "-fno-canonical-system-headers",
        "-fstack-usage",
    ]

    return [flag for flag in flags if flag not in unsupported_flags]

def _expand_flags(ctx, flags):
    return [ctx.expand_make_variables("mod_scanner_expand_flags", flag, ctx.var) for flag in flags]

def _mod_scanner_aspect_impl(target, ctx):
    # if not a C/C++ target, we are not interested
    if not CcInfo in target:
        return []

    # Ignore external targets
    if target.label.workspace_root.startswith("external"):
        return []

    # Targets with specific tags will not be scan
    if "no-mod-scan" in ctx.rule.attr.tags:
        return []

    compilation_context = target[CcInfo].compilation_context

    rule_flags = ctx.rule.attr.copts if hasattr(ctx.rule.attr, "copts") else []
    c_flags = _expand_flags(ctx, _safe_flags(_toolchain_flags(ctx, ACTION_NAMES.c_compile) + rule_flags) + ["-xc"])
    cxx_flags = _expand_flags(ctx, _safe_flags(_toolchain_flags(ctx, ACTION_NAMES.cpp_compile) + rule_flags) + ["-xc++"])

    srcs = _rule_sources(ctx)

    outputs = [
        _run_mod_scan(
            ctx,
            c_flags if src.extension == "c" else cxx_flags,
            compilation_context,
            src,
            target.label.name,
        )
        for src in srcs
    ]

    return [
        OutputGroupInfo(report = depset(direct = outputs)),
    ]

mod_scanner_aspect = aspect(
    implementation = _mod_scanner_aspect_impl,
    fragments = ["cpp"],
    attrs = {
        "_cc_toolchain": attr.label(default = Label("@bazel_tools//tools/cpp:current_cc_toolchain")),
        "_mod_scanner": attr.label(
            default = Label(":mod_scanner"),
            cfg = "exec",
            executable = True,
        ),
    },
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    required_providers = [CcInfo],
)
