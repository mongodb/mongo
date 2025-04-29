load("//bazel/toolchains/cc/mongo_windows:lib_cc_configure.bzl", "auto_configure_fail")
load(
    ":windows_cc_configure.bzl",
    "find_msvc_tool",
    "find_vc_path",
    "get_tmp_dir",
    "setup_vc_env_vars",
)

def _impl_gen_windows_toolchain_build_file(ctx):
    if "windows" not in ctx.os.name:
        ctx.file(
            "BUILD.bazel",
            "# {} not supported on this platform",
        )
        return None

    ctx.report_progress("Generating the required cc environment variables")
    vc_path = find_vc_path(ctx)
    if vc_path == None:
        auto_configure_fail("require vc path before continuing")

    vars = setup_vc_env_vars(ctx, vc_path)

    include_dirs = vars["INCLUDE"]
    if include_dirs == None:
        auto_configure_fail("failed to generate a list of cc include directories")

    lib_dirs = vars["LIB"]
    if lib_dirs == None:
        auto_configure_fail("failed to generate a list of cc library directories")

    env_path = vars["PATH"]
    if env_path == None:
        auto_configure_fail("failed to generate the cc environment paths")

    tmp_dir = get_tmp_dir(ctx)
    if tmp_dir == None:
        auto_configure_fail("temporary directory does not exist")

    substitutions = {
        "{tmp_dir}": tmp_dir,
        "{include_dirs}": include_dirs,
        "{lib_dirs}": lib_dirs,
        "{env_path}": env_path,
    }

    ctx.report_progress("Locating cc tooling in the filesystem")
    tools_subs = {
        "cl.exe": "{cl}",
        "lib.exe": "{ar}",
        "link.exe": "{ld}",
        "ml64.exe": "{ml}",
    }
    for tool_name in tools_subs:
        sub = tools_subs[tool_name]
        tool_path = find_msvc_tool(ctx, vc_path, tool_name)
        if tool_path == None:
            auto_configure_fail("locating the full path for tool %s was not found" % tool_name)
        substitutions[sub] = tool_path

    ctx.report_progress("Generating toolchain build file")
    ctx.template(
        "BUILD.bazel",
        ctx.attr.build_tpl,
        substitutions,
    )

    return None

generate_windows_toolchain_build_file = repository_rule(
    implementation = _impl_gen_windows_toolchain_build_file,
    attrs = {
        "build_tpl": attr.label(
            default = "//bazel/toolchains/cc/mongo_windows:mongo_toolchain.BUILD.tmpl",
            doc = "Label denoting the BUILD file template that gets installed in the repo.",
        ),
    },
)

def setup_mongo_windows_toolchain():
    generate_windows_toolchain_build_file(
        name = "mongo_windows_toolchain",
    )

setup_mongo_windows_toolchain_extension = module_extension(
    implementation = lambda ctx: setup_mongo_windows_toolchain(),
)
