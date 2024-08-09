HEADER_DEP_SUFFIX = "_header_dep"

def create_header_dep_impl(ctx):
    compilation_context = cc_common.create_compilation_context(
        includes = depset(transitive = [header_dep[CcInfo].compilation_context.includes for header_dep in ctx.attr.header_deps]),
        headers = depset(transitive = [header_dep[CcInfo].compilation_context.headers for header_dep in ctx.attr.header_deps]),
    )

    return CcInfo(compilation_context = compilation_context)

create_header_dep = rule(
    create_header_dep_impl,
    attrs = {
        "header_deps": attr.label_list(providers = [CcInfo]),
    },
    doc = "create header only CcInfo",
    fragments = ["cpp"],
)
