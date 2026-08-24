load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@rules_pkg//pkg:providers.bzl", "PackageFilesInfo")
load(":install_rules.bzl", "mongo_install_rule")

def _transitive_source_input_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "MongoInstallRule"
    ]
    asserts.equals(env, 1, len(actions))
    input_basenames = [file.basename for file in actions[0].inputs.to_list()]
    asserts.true(
        env,
        ctx.attr.expected_source in input_basenames,
        "a parent install action must declare original files named by child depfiles",
    )
    return analysistest.end(env)

_transitive_source_input_test = analysistest.make(
    _transitive_source_input_test_impl,
    attrs = {
        "expected_source": attr.string(mandatory = True),
    },
)

def _gdb_root_files_impl(ctx):
    env = analysistest.begin(ctx)
    package_files = analysistest.target_under_test(env)[PackageFilesInfo].dest_src_map
    asserts.equals(
        env,
        ctx.attr.expected_gdb,
        "gdb-toolchain" in package_files,
        "GDB contents did not match install_gdb",
    )
    return analysistest.end(env)

def _gdb_install_root_files_impl(ctx):
    env = analysistest.begin(ctx)
    package_files = analysistest.target_under_test(env)[PackageFilesInfo].dest_src_map
    asserts.true(
        env,
        "gdb-toolchain" in package_files,
        "install-gdb does not contain the GDB toolchain",
    )
    return analysistest.end(env)

_gdb_install_root_files_test = analysistest.make(_gdb_install_root_files_impl)

_gdb_root_files_default_test = analysistest.make(
    _gdb_root_files_impl,
    attrs = {
        "expected_gdb": attr.bool(mandatory = True),
    },
    config_settings = {
        "@@//bazel/config:install_gdb": False,
    },
)

_gdb_root_files_enabled_test = analysistest.make(
    _gdb_root_files_impl,
    attrs = {
        "expected_gdb": attr.bool(mandatory = True),
    },
    config_settings = {
        "@@//bazel/config:install_gdb": True,
    },
)

def install_rules_test_suite(name):
    source_name = name + "_source"
    native.genrule(
        name = source_name + "_gen",
        outs = [source_name],
        cmd = "touch $@",
        testonly = True,
    )
    mongo_install_rule(
        name = name + "_leaf",
        srcs = [":" + source_name + "_gen"],
        deps = [],
        debug = "",
        root_files = {},
        include_files = {},
        publish_debug_in_stripped = False,
        create_dwp = False,
        testonly = True,
    )
    mongo_install_rule(
        name = name + "_root",
        srcs = [],
        deps = [":" + name + "_leaf"],
        debug = "",
        root_files = {},
        include_files = {},
        publish_debug_in_stripped = False,
        create_dwp = False,
        testonly = True,
    )
    gdb_config_name = name + "_gdb_config"
    mongo_install_rule(
        name = gdb_config_name,
        srcs = [],
        deps = [],
        debug = "",
        root_files = select({
            "//bazel/config:install_gdb_enabled_linux": {
                "//:gdb_toolchain_folder": "lib",
            },
            "//bazel/config:install_gdb_disabled_linux": {},
            "//conditions:default": {},
        }),
        include_files = {},
        publish_debug_in_stripped = False,
        create_dwp = False,
        testonly = True,
    )
    _transitive_source_input_test(
        name = name,
        target_under_test = ":" + name + "_root",
        expected_source = source_name,
    )

    _gdb_root_files_default_test(
        name = name + "_gdb_default",
        target_under_test = ":" + gdb_config_name,
        expected_gdb = False,
        target_compatible_with = ["@platforms//os:linux"],
    )
    _gdb_root_files_enabled_test(
        name = name + "_gdb_enabled",
        target_under_test = ":" + gdb_config_name,
        expected_gdb = True,
        target_compatible_with = ["@platforms//os:linux"],
    )
    _gdb_install_root_files_test(
        name = name + "_gdb_install",
        target_under_test = "//:install-gdb",
        target_compatible_with = ["@platforms//os:linux"],
    )
