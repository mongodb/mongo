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

def create_link_dep_impl(ctx):
    deps = []
    for dep in ctx.attr.link_deps:
        if dep[CcInfo].linking_context:
            for input in dep[CcInfo].linking_context.linker_inputs.to_list():
                for library in input.libraries:
                    if library.dynamic_library and library.resolved_symlink_dynamic_library:
                        dep = library.resolved_symlink_dynamic_library.path
                        if dep not in deps:
                            deps.append(library.resolved_symlink_dynamic_library.path)
                    if library.static_library:
                        dep = library.static_library.path
                        if dep not in deps:
                            deps.append(library.static_library.path)

    link_list = ctx.actions.declare_file(ctx.attr.target_name + "_links.list")
    ctx.actions.write(
        output = link_list,
        content = "\n".join(deps),
    )

    return DefaultInfo(files = depset([link_list]))

create_link_deps = rule(
    create_link_dep_impl,
    attrs = {
        "target_name": attr.string(),
        "link_deps": attr.label_list(providers = [CcInfo]),
    },
    doc = "create a pseudo target to query link deps for",
)
