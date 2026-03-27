"""Rule to embed a binary file as a linkable object (.o) in .rodata using .incbin.

Generates a small assembly file that uses the .incbin directive to include the
binary data, then compiles it with the CC toolchain's assembler. This works with
both GCC and Clang on all platforms (Linux, macOS, Windows cross-compile).

Provides CcInfo so dependents can link the .o via deps (recommended) in addition
to using the output in linkopts/additional_linker_inputs.
"""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")

_ASM_TEMPLATE_LINUX = """\
    .section .rodata,"a"
    .global {start_sym}
    .global {end_sym}
    .global {size_sym}
    .balign 16
{start_sym}:
    .incbin "{input_path}"
{end_sym}:
{size_sym} = {end_sym} - {start_sym}
"""

# macOS uses Mach-O which prefixes C symbols with an underscore, so assembly
# symbols need the extra leading underscore. The .const section is Mach-O's
# equivalent of .rodata.
_ASM_TEMPLATE_MACOS = """\
    .section __TEXT,__const
    .global _{start_sym}
    .global _{end_sym}
    .global _{size_sym}
    .balign 16
_{start_sym}:
    .incbin "{input_path}"
_{end_sym}:
_{size_sym} = _{end_sym} - _{start_sym}
"""

def _embed_binary_obj_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    input_file = ctx.file.src
    output_file = ctx.outputs.out
    prefix = "_" + ctx.attr.symbol_prefix

    macos = ctx.attr._macos[platform_common.ConstraintValueInfo]
    is_macos = ctx.target_platform_has_constraint(macos)
    template = _ASM_TEMPLATE_MACOS if is_macos else _ASM_TEMPLATE_LINUX

    asm_file = ctx.actions.declare_file(ctx.attr.name + "_embed.S")
    ctx.actions.write(
        output = asm_file,
        content = template.format(
            start_sym = prefix + "_start",
            end_sym = prefix + "_end",
            size_sym = prefix + "_size",
            input_path = input_file.path,
        ),
    )

    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
    )

    # Use cc_common.compile to assemble the .S file. This properly sets up
    # the toolchain environment (e.g. GCC's libexec path for cc1) unlike
    # a raw ctx.actions.run with the compiler path.
    (_compilation_context, compilation_outputs) = cc_common.compile(
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        name = ctx.label.name,
        srcs = [asm_file],
        additional_inputs = [input_file],
    )

    # cc_common.compile produces objects in its own location; copy to the
    # expected output path.
    objs = compilation_outputs.objects
    if not objs:
        objs = compilation_outputs.pic_objects
    ctx.actions.symlink(output = output_file, target_file = objs[0])

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
            doc = "Output linkable object (.o) with content in .rodata.",
        ),
        "symbol_prefix": attr.string(
            mandatory = True,
            doc = "Short name for embedded symbols; produces _<symbol_prefix>_start, _end, _size.",
        ),
        "_cc_toolchain": attr.label(
            default = "@bazel_tools//tools/cpp:current_cc_toolchain",
        ),
        "_macos": attr.label(default = "@platforms//os:macos"),
    },
    doc = "Embeds a binary file as a linkable object using .incbin via the CC toolchain.",
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
