"""Aspect-based compile_commands fragment generation."""

load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")

_SOURCE_EXTENSIONS = {
    "c": True,
    "cc": True,
    "cpp": True,
    "cxx": True,
    "c++": True,
    "C": True,
}

CompileCommandInfo = provider(
    "Transitive compile_commands fragment files.",
    fields = {
        "files": "depset of compile command fragments",
        "required_inputs": "depset of generated compile inputs that must be materialized",
    },
)

def _is_cpp_source(src):
    return src.extension in _SOURCE_EXTENSIONS

def _rule_sources(ctx):
    srcs = []
    if hasattr(ctx.rule.files, "srcs"):
        srcs.extend(ctx.rule.files.srcs)
    if hasattr(ctx.rule.file, "src") and ctx.rule.file.src:
        srcs.append(ctx.rule.file.src)
    elif hasattr(ctx.rule.attr, "srcs"):
        for src in ctx.rule.attr.srcs:
            srcs.extend(src.files.to_list())

    seen = {}
    filtered = []
    for src in srcs:
        if not _is_cpp_source(src):
            continue
        if src.path in seen:
            continue
        seen[src.path] = True
        filtered.append(src)
    return filtered

def _expand_flags(ctx, flags):
    needs_location_expansion = False
    needs_make_expansion = False
    for flag in flags:
        if "$(" in flag:
            needs_make_expansion = True
            if "$(location" in flag:
                needs_location_expansion = True

    if not needs_make_expansion:
        return flags

    location_targets = []
    if needs_location_expansion:
        seen_labels = {}
        for attr_name in [
            "srcs",
            "hdrs",
            "textual_hdrs",
            "deps",
            "implementation_deps",
            "additional_compiler_inputs",
            "data",
            "binary_with_debug",
        ]:
            if not hasattr(ctx.rule.attr, attr_name):
                continue
            attr_value = getattr(ctx.rule.attr, attr_name)
            if type(attr_value) == "list":
                values = attr_value
            elif attr_value != None:
                values = [attr_value]
            else:
                values = []

            for value in values:
                if not hasattr(value, "label"):
                    continue
                label = str(value.label)
                if label in seen_labels:
                    continue
                seen_labels[label] = True
                location_targets.append(value)

    expanded = []
    for flag in flags:
        if needs_location_expansion and "$(location" in flag:
            flag = ctx.expand_location(flag, location_targets)
        if "$(" in flag:
            flag = ctx.expand_make_variables("compiledb_expand_flags", flag, ctx.var)
        expanded.append(flag)
    return expanded

def _compile_variables(cc_toolchain, feature_configuration, compilation_context, user_compile_flags):
    return cc_common.create_compile_variables(
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        user_compile_flags = user_compile_flags,
        include_directories = compilation_context.includes,
        quote_include_directories = compilation_context.quote_includes,
        system_include_directories = depset(
            transitive = [
                compilation_context.system_includes,
                compilation_context.external_includes,
            ],
        ),
        framework_include_directories = compilation_context.framework_includes,
        preprocessor_defines = depset(
            transitive = [
                compilation_context.defines,
                compilation_context.local_defines,
            ],
        ),
    )

def _rule_compile_flags(ctx):
    common_flags = []
    cxx_flags = []

    if hasattr(ctx.rule.attr, "copts"):
        common_flags.extend(ctx.rule.attr.copts)
    if hasattr(ctx.rule.attr, "cxxopts"):
        cxx_flags.extend(ctx.rule.attr.cxxopts)

    return common_flags, cxx_flags

def _requested_and_unsupported_features(ctx):
    requested_features = list(ctx.features)
    unsupported_features = list(ctx.disabled_features)

    if hasattr(ctx.rule.attr, "features"):
        for feature in ctx.rule.attr.features:
            if feature.startswith("-"):
                unsupported_features.append(feature[1:])
            else:
                requested_features.append(feature)

    return requested_features, unsupported_features

def _toolchain_flags(feature_configuration, action_name, compile_variables):
    return cc_common.get_memory_inefficient_command_line(
        feature_configuration = feature_configuration,
        action_name = action_name,
        variables = compile_variables,
    )

def _compiler_path(cc_toolchain):
    compiler = cc_toolchain.compiler_executable
    if hasattr(compiler, "path"):
        return compiler.path
    return compiler

def _should_materialize_artifact(path):
    return path.startswith("bazel-out/")

def _collect_dep_compile_command_info(ctx):
    output_files = []
    required_inputs = []
    for attr_name in ["deps", "implementation_deps", "binary_with_debug"]:
        if not hasattr(ctx.rule.attr, attr_name):
            continue

        attr_value = getattr(ctx.rule.attr, attr_name)
        if type(attr_value) == "list":
            deps = attr_value
        else:
            deps = [attr_value]

        for dep in deps:
            if dep != None and CompileCommandInfo in dep:
                output_files.append(dep[CompileCommandInfo].files)
                required_inputs.append(dep[CompileCommandInfo].required_inputs)

    return depset(transitive = output_files), depset(transitive = required_inputs)

def _required_compile_inputs(ctx, compilation_context, srcs):
    direct = []
    seen = {}

    if hasattr(ctx.rule.files, "srcs"):
        for artifact in ctx.rule.files.srcs:
            if _should_materialize_artifact(artifact.path) and artifact.path not in seen:
                seen[artifact.path] = True
                direct.append(artifact)

    if hasattr(ctx.rule.file, "src") and ctx.rule.file.src:
        artifact = ctx.rule.file.src
        if _should_materialize_artifact(artifact.path) and artifact.path not in seen:
            seen[artifact.path] = True
            direct.append(artifact)

    for src in srcs:
        if _should_materialize_artifact(src.path) and src.path not in seen:
            seen[src.path] = True
            direct.append(src)

    for header in compilation_context.headers.to_list():
        if _should_materialize_artifact(header.path) and header.path not in seen:
            seen[header.path] = True
            direct.append(header)

    for attr_name in ["hdrs", "textual_hdrs", "additional_compiler_inputs"]:
        if not hasattr(ctx.rule.files, attr_name):
            continue
        for artifact in getattr(ctx.rule.files, attr_name):
            if _should_materialize_artifact(artifact.path) and artifact.path not in seen:
                seen[artifact.path] = True
                direct.append(artifact)

    return depset(direct = direct)

def _package_root(label):
    parts = []
    if label.workspace_root:
        parts.append(label.workspace_root)
    if label.package:
        parts.append(label.package)
    return "/".join(parts)

def _hex32(val):
    v = val & 0xFFFFFFFF
    s = "%x" % v
    return ("0" * (8 - len(s))) + s

def _fragment_file_id(target, src):
    return src.basename + "." + _hex32(hash(src.path)) + "." + _hex32(hash(str(target.label)))

def _output_path(ctx, target, src):
    package_root = _package_root(target.label)
    parts = [ctx.bin_dir.path]
    if package_root:
        parts.append(package_root)
    src_parts = src.short_path.split("/")
    parts.extend([
        "_compiledb_objs",
        target.label.name,
    ])
    parts.extend(src_parts[:-1])
    parts.append(src_parts[-1] + ".o")
    return "/".join(parts)

def _rewrite_msvc_external_include_flags(args, compilation_context):
    external_include_paths = {
        path: True
        for path in compilation_context.external_includes.to_list()
    }
    if not external_include_paths:
        return args

    rewritten = []
    skip_next = False
    for index in range(len(args)):
        if skip_next:
            skip_next = False
        else:
            arg = args[index]
            if arg == "/I" and index + 1 < len(args):
                include_path = args[index + 1]
                if include_path in external_include_paths:
                    rewritten.extend([
                        "/external:I" + include_path,
                        "/external:W0",
                    ])
                else:
                    rewritten.extend([arg, include_path])
                skip_next = True
            elif arg.startswith("/I") and arg[2:] in external_include_paths:
                rewritten.extend([
                    "/external:I" + arg[2:],
                    "/external:W0",
                ])
            else:
                rewritten.append(arg)

    return rewritten

def _command_line_args(target, src, compiler, is_msvc, toolchain_flags, compilation_context, ctx):
    output_path = _output_path(ctx, target, src)

    args = [compiler]
    args.extend(toolchain_flags)
    if is_msvc:
        args = _rewrite_msvc_external_include_flags(args, compilation_context)

    if is_msvc:
        args.extend(["/c", src.path, "/Fo" + output_path])
    else:
        args.extend(["-c", src.path, "-o", output_path])

    return args, output_path

def _emit_compile_command(ctx, target, src, command_line, output_path):
    file_id = _fragment_file_id(target, src)
    output = ctx.actions.declare_file("compiledb/" + file_id + ".compile_command.json")
    ctx.actions.write(
        output = output,
        content = json.encode({
            "file": src.path,
            "arguments": command_line,
            "output": output_path,
            "target": str(target.label),
        }),
    )
    return output

def _compiledb_aspect_impl(target, ctx):
    dep_outputs, dep_required_inputs = _collect_dep_compile_command_info(ctx)
    if CcInfo not in target:
        return [
            CompileCommandInfo(files = dep_outputs, required_inputs = dep_required_inputs),
            OutputGroupInfo(compiledb_report = depset(transitive = [dep_outputs, dep_required_inputs])),
        ]

    compilation_context = target[CcInfo].compilation_context
    srcs = _rule_sources(ctx)
    if not srcs:
        return [
            CompileCommandInfo(files = dep_outputs, required_inputs = dep_required_inputs),
            OutputGroupInfo(compiledb_report = depset(transitive = [dep_outputs, dep_required_inputs])),
        ]

    cc_toolchain = find_cpp_toolchain(ctx)
    requested_features, unsupported_features = _requested_and_unsupported_features(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = requested_features,
        unsupported_features = unsupported_features,
    )
    rule_compile_flags, rule_cxx_flags = _rule_compile_flags(ctx)
    c_user_compile_flags = _expand_flags(
        ctx,
        ctx.fragments.cpp.conlyopts + ctx.fragments.cpp.copts + rule_compile_flags,
    )
    cpp_user_compile_flags = _expand_flags(
        ctx,
        ctx.fragments.cpp.cxxopts + ctx.fragments.cpp.copts + rule_compile_flags + rule_cxx_flags,
    )
    c_compile_variables = _compile_variables(
        cc_toolchain,
        feature_configuration,
        compilation_context,
        c_user_compile_flags,
    )
    cpp_compile_variables = _compile_variables(
        cc_toolchain,
        feature_configuration,
        compilation_context,
        cpp_user_compile_flags,
    )
    compiler = _compiler_path(cc_toolchain)
    is_msvc = compiler.endswith("cl.exe") or compiler.endswith("/cl") or compiler.endswith("\\cl.exe")
    c_toolchain_flags = None
    cpp_toolchain_flags = None
    outputs = []
    for src in srcs:
        if src.extension == "c":
            if c_toolchain_flags == None:
                c_toolchain_flags = _toolchain_flags(
                    feature_configuration,
                    ACTION_NAMES.c_compile,
                    c_compile_variables,
                )
            toolchain_flags = c_toolchain_flags
        else:
            if cpp_toolchain_flags == None:
                cpp_toolchain_flags = _toolchain_flags(
                    feature_configuration,
                    ACTION_NAMES.cpp_compile,
                    cpp_compile_variables,
                )
            toolchain_flags = cpp_toolchain_flags

        command_line, output_path = _command_line_args(
            target,
            src,
            compiler,
            is_msvc,
            toolchain_flags,
            compilation_context,
            ctx,
        )
        outputs.append(
            _emit_compile_command(
                ctx,
                target,
                src,
                command_line,
                output_path,
            ),
        )
    all_outputs = depset(direct = outputs, transitive = [dep_outputs])
    required_inputs = depset(
        transitive = [
            dep_required_inputs,
            _required_compile_inputs(ctx, compilation_context, srcs),
        ],
    )

    return [
        CompileCommandInfo(files = all_outputs, required_inputs = required_inputs),
        OutputGroupInfo(compiledb_report = depset(transitive = [all_outputs, required_inputs])),
    ]

compiledb_aspect = aspect(
    implementation = _compiledb_aspect_impl,
    fragments = ["cpp"],
    attrs = {
        "_cc_toolchain": attr.label(default = Label("@bazel_tools//tools/cpp:current_cc_toolchain")),
    },
    toolchains = [
        "@bazel_tools//tools/cpp:toolchain_type",
    ],
    attr_aspects = ["deps", "implementation_deps", "binary_with_debug"],
    required_providers = [CcInfo],
)
