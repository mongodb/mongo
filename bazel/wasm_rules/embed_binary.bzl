"""Rule to embed a binary file as a linkable ELF object (.o) in .rodata using objcopy.

Uses the Cc toolchain's objcopy and target architecture so the output object
matches the current build's ABI and can be linked into a native binary.

Provides CcInfo so dependents can link the .o via deps (recommended) in addition
to using the output in linkopts/additional_linker_inputs.
"""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")

def _obj_fmt_and_bfd_arch(ctx):
    """Maps platform CPU constraints to objcopy -O (BFD format) and -B (BFD arch)."""
    x86_64 = ctx.attr._x86_64[platform_common.ConstraintValueInfo]
    aarch64 = ctx.attr._aarch64[platform_common.ConstraintValueInfo]
    arm64 = ctx.attr._arm64[platform_common.ConstraintValueInfo]

    if ctx.target_platform_has_constraint(x86_64):
        return ("elf64-x86-64", "i386:x86-64")
    if ctx.target_platform_has_constraint(aarch64) or ctx.target_platform_has_constraint(arm64):
        return ("elf64-littleaarch64", "aarch64")
    fail("embed_binary_obj is only supported on x86_64 and aarch64/arm64")

def _binary_symbol_prefix(path):
    """Computes the symbol prefix objcopy uses for -I binary (path with non-alnum -> _)."""
    result = "_binary_"
    for i in range(len(path)):
        c = path[i]
        if (c.isalnum() or c == "_"):
            result += c
        else:
            result += "_"
    return result

def _embed_binary_obj_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    input_file = ctx.file.src
    output_file = ctx.outputs.out

    obj_fmt, bfd_arch = _obj_fmt_and_bfd_arch(ctx)
    prefix = _binary_symbol_prefix(input_file.path)
    new_prefix = "_" + ctx.attr.symbol_prefix

    # .data is renamed to .rodata just for semantic purposes, data using this rule should be read only
    # --redefine symbol is used or else the symbol name is based on the path to the obj
    # this will expose the symbols _binary_+<chosen symbol name>+_start and
    # this will expose the symbols _binary_+<chosen symbol name>+_end which can be used to read the data
    args = [
        "-I",
        "binary",
        "-O",
        obj_fmt,
        "-B",
        bfd_arch,
        "--rename-section",
        ".data=.rodata",
        "--redefine-sym",
        prefix + "_start=" + new_prefix + "_start",
        "--redefine-sym",
        prefix + "_end=" + new_prefix + "_end",
        "--redefine-sym",
        prefix + "_size=" + new_prefix + "_size",
        input_file.path,
        output_file.path,
    ]

    ctx.actions.run(
        executable = cc_toolchain.objcopy_executable,
        inputs = depset([input_file], transitive = [cc_toolchain.all_files]),
        outputs = [output_file],
        arguments = args,
        mnemonic = "EmbedBinaryObj",
        progress_message = "Embedding %s as ELF object" % input_file.short_path,
    )

    # Provide CcInfo so dependents can link this .o via deps (ensures the object
    # is on the link line through the normal C++ dependency path).
    linker_input = cc_common.create_linker_input(
        owner = ctx.label,
        user_link_flags = [output_file.path],
        additional_inputs = depset([output_file]),
    )
    linking_context = cc_common.create_linking_context(
        linker_inputs = depset(direct = [linker_input]),
    )

    return [
        DefaultInfo(files = depset([output_file])),
        CcInfo(linking_context = linking_context),
    ]

embed_binary_obj = rule(
    implementation = _embed_binary_obj_impl,
    attrs = {
        "src": attr.label(
            allow_single_file = True,
            mandatory = True,
            doc = "Binary file to embed (e.g. .wasm or .cwasm).",
        ),
        "out": attr.output(
            mandatory = True,
            doc = "Output linkable ELF object (.o) with content in .rodata.",
        ),
        "symbol_prefix": attr.string(
            mandatory = True,
            doc = "Short name for embedded symbols; produces _<symbol_prefix>_start, _end, _size.",
        ),
        "_cc_toolchain": attr.label(
            default = "@bazel_tools//tools/cpp:current_cc_toolchain",
        ),
        "_x86_64": attr.label(default = "@platforms//cpu:x86_64"),
        "_aarch64": attr.label(default = "@platforms//cpu:aarch64"),
        "_arm64": attr.label(default = "@platforms//cpu:arm64"),
    },
    doc = "Embeds a binary file as a linkable ELF object using the CC toolchain's objcopy.",
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    fragments = ["cpp"],
)

"""Rule to embed a binary file as a Windows resource using rc.exe.

Generates an .rc file referencing the binary, compiles it to a .res with rc.exe,
and provides CcInfo so dependents can link it via deps.

At runtime, use FindResource/LoadResource/LockResource/SizeofResource to access
the data, or use the helper in embedded_resource.h.
"""

def _embed_binary_rc_impl(ctx):
    input_file = ctx.file.src
    rc_file = ctx.actions.declare_file(ctx.attr.name + ".rc")
    res_file = ctx.outputs.out

    # The resource type and name used to look up the data at runtime.
    resource_name = ctx.attr.resource_name

    # Generate an .rc file that references the binary as RCDATA.
    ctx.actions.write(
        output = rc_file,
        content = '{resource_name} RCDATA "{input_path}"\n'.format(
            resource_name = resource_name,
            input_path = input_file.path.replace("/", "\\\\"),
        ),
    )

    args = ctx.actions.args()
    args.add("/nologo")
    args.add("/fo" + res_file.path)
    args.add(rc_file.path)

    ctx.actions.run(
        executable = ctx.executable.rc,
        arguments = [args],
        inputs = [rc_file, input_file],
        outputs = [res_file],
        mnemonic = "EmbedBinaryRC",
        progress_message = "Embedding %s as Windows resource" % input_file.short_path,
    )

    linker_input = cc_common.create_linker_input(
        owner = ctx.label,
        user_link_flags = [res_file.path],
        additional_inputs = depset([res_file]),
    )
    linking_context = cc_common.create_linking_context(
        linker_inputs = depset(direct = [linker_input]),
    )

    return [
        DefaultInfo(files = depset([res_file])),
        CcInfo(linking_context = linking_context),
    ]

embed_binary_rc = rule(
    implementation = _embed_binary_rc_impl,
    attrs = {
        "src": attr.label(
            allow_single_file = True,
            mandatory = True,
            doc = "Binary file to embed (e.g. .wasm or .cwasm).",
        ),
        "out": attr.output(
            mandatory = True,
            doc = "Output .res file.",
        ),
        "resource_name": attr.string(
            mandatory = True,
            doc = "Resource name used in the .rc file and at runtime with FindResource.",
        ),
        "rc": attr.label(
            executable = True,
            cfg = "exec",
            allow_files = True,
            default = "@mongo_windows_toolchain//:rc",
            doc = "The rc.exe compiler.",
        ),
    },
    provides = [CcInfo],
    doc = "Embeds a binary file as a Windows resource (.res) using rc.exe.",
)
