load("//bazel/toolchains/cc/mongo_windows:lib_cc_configure.bzl", "auto_configure_fail")
load(
    ":windows_cc_configure.bzl",
    "find_msvc_tool",
    "find_vc_path",
    "get_tmp_dir",
    "has_atl_installed",
    "has_win_sdk_installed",
    "is_msvc_exists",
    "is_msvc_version_set",
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
        auto_configure_fail("Microsoft Visual Studio (VS) is not installed. Please install VS with VC, ATL and SDK support.")

    # Verify that the VC build tools exists.
    msvc_exists, msvc_version = is_msvc_exists(ctx, vc_path)
    if msvc_exists:
        print("Using Microsoft VC version %s" % msvc_version)
    else:
        message = "Microsoft Visual C++ build tools %s could not be found.\n" % msvc_version
        if is_msvc_version_set(ctx):
            message += "Please make sure that the BAZEL_VC_FULL_VERSION in //.bazelrc is set to a correct version\n"
            message += "under \"%s\\Tools\\MSVC\" directory\n" % vc_path
            message += "or unset out the BAZEL_VC_FULL_VERSION variable to use the default installed version."
        else:
            message += "Please make sure that Visual C++ is installed on the host along with ATL support."
        auto_configure_fail(message)

    vars = setup_vc_env_vars(ctx, vc_path)

    # verify that in the include_dirs, the ATL and Windows SDK are installed.
    has_atl_installed(ctx, vc_path, vars)
    has_win_sdk_installed(ctx, vars)

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

    # Save all the information to the file in bazel out to be used for debugging
    # purpose.
    ctx.file("windows_toolchain_config.json", json.encode(substitutions), executable = False)

    ctx.report_progress("Generating toolchain build file")
    ctx.template(
        "BUILD.bazel",
        ctx.attr.build_tpl,
        substitutions,
    )

    return None

generate_windows_toolchain_build_file = repository_rule(
    environ = [
        "BAZEL_VC_FULL_VERSION",  # Force re-compute if the user changed the version of MS compiler.
    ],
    implementation = _impl_gen_windows_toolchain_build_file,
    attrs = {
        "build_tpl": attr.label(
            default = "//bazel/toolchains/cc/mongo_windows:mongo_toolchain.BUILD.tmpl",
            doc = "Label denoting the BUILD file template that gets installed in the repo.",
        ),
    },
    configure = True,
    local = True,
)

def setup_mongo_windows_toolchain():
    generate_windows_toolchain_build_file(
        name = "mongo_windows_toolchain",
    )

setup_mongo_windows_toolchain_extension = module_extension(
    implementation = lambda ctx: setup_mongo_windows_toolchain(),
)
