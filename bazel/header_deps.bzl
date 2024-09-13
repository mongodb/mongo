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
                        dep = library.resolved_symlink_dynamic_library.path
                        if dep not in deps:
                            deps.append(library.resolved_symlink_dynamic_library.path)
                    if library.static_library:
                        dep = library.static_library.path
                        if dep not in deps:
                            deps.append(library.static_library.path)

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    link_list = ctx.actions.declare_file(ctx.attr.target_name + "_links.list")
    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = [link_list],
        inputs = depset(transitive = [ctx.attr._link_list_writer.files, python.files]),
        arguments = [ctx.attr._link_list_writer.files.to_list()[0].path, link_list.path] + deps,
        mnemonic = "LinkListFile",
    )

    return DefaultInfo(files = depset([link_list]))

create_link_deps = rule(
    create_link_dep_impl,
    attrs = {
        "target_name": attr.string(),
        "link_deps": attr.label_list(providers = [CcInfo]),
        "_link_list_writer": attr.label(allow_single_file = True, default = "//bazel:scons_link_list.py"),
    },
    doc = "create a psuedo target to query link deps for",
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
)
