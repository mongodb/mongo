load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")

SymbolInfo = provider(
    fields = {
        "symbol_file": "depset of files containing symbol info",
    },
)

def _collect_dep_symbol_files_from_deps(ctx):
    if not hasattr(ctx.rule.attr, "deps"):
        return depset()
    return depset(transitive = [
        dep[SymbolInfo].symbol_file
        for dep in ctx.rule.attr.deps
        if SymbolInfo in dep
    ])

def _collect_cc_objects(cc_info):
    objs = []
    for linker_input in cc_info.linking_context.linker_inputs.to_list():
        for lib in linker_input.libraries:
            if lib.objects:
                objs += lib.objects
            if lib.pic_objects:
                objs += lib.pic_objects
    return objs

def _has_skip_tag(ctx):
    return "skip_symbol_check" in getattr(ctx.rule.attr, "tags", [])

def _has_check_tag(ctx):
    # only do extraction/check on targets tagged with this
    return "check_symbol_target" in getattr(ctx.rule.attr, "tags", [])

def _pick_cc_info(target, ctx):
    # Prefer the target's own CcInfo if present
    if CcInfo in target:
        return target[CcInfo]
    return None

def symbol_checker_aspect_impl(target, ctx):
    # Always forward deps’ symbol files so downstream checks see them
    if hasattr(ctx.rule.attr, "binary_with_debug"):
        cc_tgt = ctx.rule.attr.binary_with_debug
        dep_sym_files = depset(transitive = [cc_tgt[SymbolInfo].symbol_file])
        return [
            SymbolInfo(symbol_file = dep_sym_files),
            OutputGroupInfo(symbol_checker = depset()),
        ]
    else:
        dep_sym_files = _collect_dep_symbol_files_from_deps(ctx)

    # Gate heavy work on the presence of the tag
    if not _has_check_tag(ctx):
        return [
            SymbolInfo(symbol_file = dep_sym_files),
            OutputGroupInfo(symbol_checker = depset()),
        ]

    cc_info = _pick_cc_info(target, ctx)
    if cc_info == None:
        # Not a C++ target (or no usable CcInfo) — just forward
        return [
            SymbolInfo(symbol_file = dep_sym_files),
            OutputGroupInfo(symbol_checker = depset()),
        ]

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    cc_toolchain = find_cpp_toolchain(ctx)
    nm_bin = cc_toolchain.nm_executable

    objs = _collect_cc_objects(cc_info)
    if not objs:
        return [
            SymbolInfo(symbol_file = dep_sym_files),
            OutputGroupInfo(symbol_checker = depset()),
        ]

    # --- extract ---
    out = ctx.actions.declare_file(target.label.name + "_symbols.sym")
    extract_args = ctx.actions.args()
    extract_args.add(ctx.attr._extractor.files.to_list()[0])
    extract_args.add("--out")
    extract_args.add(out)
    extract_args.add("--nm")
    extract_args.add(nm_bin)
    extract_args.add_all([o.path for o in objs], before_each = "--obj")

    extract_inputs = depset(transitive = [
        ctx.attr._extractor.files,
        python.files,
        cc_toolchain.all_files,
        depset(objs),
    ])

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = [out],
        inputs = extract_inputs,
        arguments = [extract_args],
        mnemonic = "SymbolExtractor",
    )

    # --- check ---
    check = ctx.actions.declare_file(target.label.name + "_checked")
    check_args = ctx.actions.args()
    check_args.add(ctx.attr._checker.files.to_list()[0])
    check_args.add("--sym")
    check_args.add(out)
    check_args.add("--out")
    check_args.add(check)
    check_args.add("--label")
    check_args.add(str(target.label))

    if _has_skip_tag(ctx):
        check_args.add("--skip")

    for dep_sym in dep_sym_files.to_list():
        check_args.add("--dep")
        check_args.add(dep_sym)

    check_inputs = depset(transitive = [
        ctx.attr._checker.files,
        python.files,
        depset([out]),
        dep_sym_files,
    ])

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = [check],
        inputs = check_inputs,
        arguments = [check_args],
        mnemonic = "SymbolChecker",
    )

    return [
        SymbolInfo(
            symbol_file = depset(direct = [out], transitive = [dep_sym_files]),
        ),
        OutputGroupInfo(symbol_checker = depset([check])),
    ]

symbol_checker_aspect = aspect(
    implementation = symbol_checker_aspect_impl,
    attrs = {
        "_extractor": attr.label(
            default = "//bazel/symbol_checker:symbol_extractor.py",
            allow_single_file = True,
        ),
        "_checker": attr.label(
            default = "//bazel/symbol_checker:symbol_checker.py",
            allow_single_file = True,
        ),
    },
    toolchains = [
        "@bazel_tools//tools/python:toolchain_type",
        "@bazel_tools//tools/cpp:toolchain_type",
    ],
    attr_aspects = ["deps", "binary_with_debug"],
)
