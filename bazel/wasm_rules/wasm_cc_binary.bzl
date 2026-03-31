"""Rule to link a WASM binary from cc_library deps using the WASI toolchain.

Applies the wasi_transition internally to all deps, so they are compiled under
the WASI CC toolchain. Sources should be in cc_library targets listed as deps,
not compiled directly by this rule.
"""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain", "use_cpp_toolchain")
load("//bazel/toolchains/cc/mongo_wasm/toolchain:wasi_transition.bzl", "wasi_transition")

# Flags that leak from host-oriented deps but are invalid for WASI linking.
_WASI_INVALID_LINK_FLAGS = ["-pthread", "-ldl", "-lrt", "-lm"]

def _filter_linking_contexts(ctx, linking_contexts):
    """Rebuild linking contexts with host-specific flags removed."""
    filtered_linker_inputs = []
    for lc in linking_contexts:
        for li in lc.linker_inputs.to_list():
            flags = [f for f in li.user_link_flags if f not in _WASI_INVALID_LINK_FLAGS]
            filtered_li = cc_common.create_linker_input(
                owner = li.owner,
                libraries = depset(li.libraries),
                user_link_flags = flags,
                additional_inputs = depset(li.additional_inputs),
            )
            filtered_linker_inputs.append(filtered_li)

    return cc_common.create_linking_context(
        linker_inputs = depset(filtered_linker_inputs),
    )

def _wasm_cc_binary_impl(ctx):
    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    # Collect and filter linking contexts from all deps.
    dep_linking_contexts = [
        dep[CcInfo].linking_context
        for dep in ctx.attr.deps
        if CcInfo in dep
    ]
    filtered_context = _filter_linking_contexts(ctx, dep_linking_contexts)

    # Gather additional linker inputs (e.g. api_component_type.o).
    additional_inputs = depset(ctx.files.additional_linker_inputs)
    user_link_flags = list(ctx.attr.linkopts)
    for f in ctx.files.additional_linker_inputs:
        user_link_flags.append(f.path)

    # Link into a WASM binary. We pass an empty compilation_outputs since
    # all objects come from deps' linking contexts.
    compilation_outputs = cc_common.create_compilation_outputs()

    linking_outputs = cc_common.link(
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        name = ctx.attr.out,
        compilation_outputs = compilation_outputs,
        linking_contexts = [filtered_context],
        user_link_flags = user_link_flags,
        additional_inputs = additional_inputs,
        output_type = "executable",
    )

    output = linking_outputs.executable
    return [
        DefaultInfo(
            files = depset([output]),
            executable = output,
        ),
    ]

wasm_cc_binary = rule(
    implementation = _wasm_cc_binary_impl,
    attrs = {
        "deps": attr.label_list(
            providers = [CcInfo],
            cfg = wasi_transition,
            doc = "C++ library dependencies. Built under the WASI transition.",
        ),
        "additional_linker_inputs": attr.label_list(
            allow_files = True,
            cfg = wasi_transition,
            doc = "Extra files passed to the linker (e.g. prebuilt .o files).",
        ),
        "linkopts": attr.string_list(
            doc = "Extra flags for the link step.",
        ),
        "out": attr.string(
            mandatory = True,
            doc = "Output filename (e.g. 'foo.wasm').",
        ),
    },
    cfg = wasi_transition,
    toolchains = use_cpp_toolchain(),
    fragments = ["cpp"],
    doc = "Links a WASM binary from cc_library deps under the WASI toolchain.",
)
