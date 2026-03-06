"""Expose C++ link information (objects/libs/flags) via response files for genrules."""

def _as_list(x):
    if x == None:
        return []
    t = type(x)
    if t == "depset":
        return x.to_list()
    if t == "list" or t == "tuple":
        return x

    # File or string or other singletons
    return [x]

def _collect_files_from_library(lib):
    """Return (objects, libs) from a LibraryToLink in a Bazel-version-tolerant way."""
    objs = []

    # Object files (may be depsets)
    if hasattr(lib, "objects"):
        objs += _as_list(lib.objects)
    if hasattr(lib, "pic_objects"):
        objs += _as_list(lib.pic_objects)

    libs = []

    # Prefer static archives; include others if present
    for attr in (
        "static_library",
        "pic_static_library",
        "dynamic_library",
        "interface_library",
    ):
        if hasattr(lib, attr):
            val = getattr(lib, attr)
            libs += _as_list(val)
    return objs, libs

def _cc_linkset_impl(ctx):
    linking_ctxs = [d[CcInfo].linking_context for d in ctx.attr.deps]
    merged = cc_common.merge_linking_contexts(linking_contexts = linking_ctxs)

    objs_ordered = []
    libs_ordered = []
    flags_ordered = []

    # Iterate in Bazel's link order
    linker_inputs_list = merged.linker_inputs.to_list()

    # Collect from all deps - include both direct and transitive
    for li in linker_inputs_list:
        if hasattr(li, "user_link_flags"):
            flags_ordered += list(li.user_link_flags)
        for lib in li.libraries:
            o, libs = _collect_files_from_library(lib)

            # Collect all objects and libraries
            objs_ordered += o
            libs_ordered += libs

    noncode = []
    if hasattr(merged, "non_code_inputs"):
        noncode = merged.non_code_inputs.to_list()

    # Emit response files
    objs_rsp = ctx.actions.declare_file(ctx.label.name + ".objects.rsp")
    libs_rsp = ctx.actions.declare_file(ctx.label.name + ".libs.rsp")
    flags_rsp = ctx.actions.declare_file(ctx.label.name + ".flags.rsp")

    def _write_list(out, items):
        lines = []
        for it in items:
            if hasattr(it, "path"):
                lines.append(it.path)
            else:
                lines.append(str(it))
        content = "\n".join(lines)
        if content:
            content += "\n"
        ctx.actions.write(out, content)

    _write_list(objs_rsp, objs_ordered)
    _write_list(libs_rsp, libs_ordered + noncode)
    _write_list(flags_rsp, flags_ordered + ctx.attr.extra_flags)

    # Expose everything so genrules can use $(locations :target)
    all_files = depset(
        direct = [objs_rsp, libs_rsp, flags_rsp] + objs_ordered + libs_ordered + noncode,
    )

    return [
        DefaultInfo(files = all_files),
        OutputGroupInfo(
            rsp = depset([objs_rsp, libs_rsp, flags_rsp]),
            objects = depset(objs_ordered),
            libs = depset(libs_ordered),
        ),
    ]

cc_linkset = rule(
    implementation = _cc_linkset_impl,
    attrs = {
        "deps": attr.label_list(providers = [CcInfo]),
        "extra_flags": attr.string_list(default = []),
        "_cc_toolchain": attr.label(
            default = Label("@bazel_tools//tools/cpp:current_cc_toolchain"),
        ),
    },
    toolchains = ["@bazel_tools//tools/cpp:toolchain_type"],
    fragments = ["cpp"],
)
