HEADER_DEP_SUFFIX = "_header_dep"
LINK_DEP_SUFFIX = "_link_dep"

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

def create_link_dep_impl(ctx):
    deps = []
    for dep in ctx.attr.link_deps:
        if dep[CcInfo].linking_context:
            for input in dep[CcInfo].linking_context.linker_inputs.to_list():
                for library in input.libraries:
                    if library.dynamic_library:
                        deps.append(depset([library.resolved_symlink_dynamic_library]))
                    if library.static_library:
                        deps.append(depset([library.static_library]))
    return DefaultInfo(files = depset(transitive = deps))

create_link_deps = rule(
    create_link_dep_impl,
    attrs = {
        "link_deps": attr.label_list(providers = [CcInfo]),
    },
    doc = "create a psuedo target to query link deps for",
    fragments = ["cpp"],
)
