load("@bazel_skylib//lib:unittest.bzl", "analysistest", "asserts")
load("@rules_pkg//pkg:providers.bzl", "PackageFilesInfo")
load("//bazel:separate_debug.bzl", "TagInfo")
load(":install_rules.bzl", "MongoInstallInfo", "mongo_install_rule")
load(":providers.bzl", "TestBinaryInfo")

def _generated_file_impl(ctx):
    output = ctx.actions.declare_file(ctx.attr.output)
    ctx.actions.write(output = output, content = "test input\n")
    providers = [DefaultInfo(files = depset([output]))]
    if ctx.attr.test_binary:
        providers.append(TestBinaryInfo(test_binaries = depset([output])))
    return providers

_generated_file = rule(
    implementation = _generated_file_impl,
    attrs = {
        "output": attr.string(mandatory = True),
        "test_binary": attr.bool(),
    },
)

def _generated_test_with_data_impl(ctx):
    output = ctx.actions.declare_file(ctx.attr.output)
    ctx.actions.write(output = output, content = "test input\n")
    return [
        DefaultInfo(
            files = depset([output]),
            data_runfiles = ctx.runfiles(files = ctx.files.data),
        ),
        TagInfo(tags = ["mongo_unittest"]),
    ]

_generated_test_with_data = rule(
    implementation = _generated_test_with_data_impl,
    attrs = {
        "output": attr.string(mandatory = True),
        "data": attr.label_list(allow_files = True),
    },
)

def _generated_directory_impl(ctx):
    output = ctx.actions.declare_directory(ctx.attr.output)
    ctx.actions.run_shell(
        outputs = [output],
        arguments = [output.path],
        command = "mkdir -p \"$1\"",
    )
    return [DefaultInfo(files = depset([output]))]

_generated_directory = rule(
    implementation = _generated_directory_impl,
    attrs = {
        "output": attr.string(mandatory = True),
    },
)

def _install_rule(name, srcs = [], deps = [], root_files = {}, include_files = {}, tags = []):
    mongo_install_rule(
        name = name,
        srcs = srcs,
        deps = deps,
        debug = "",
        root_files = root_files,
        include_files = include_files,
        publish_debug_in_stripped = False,
        create_dwp = False,
        tags = tags,
        testonly = True,
    )

def _install_actions(env):
    return [
        action
        for action in analysistest.target_actions(env)
        if action.mnemonic == "MongoInstallRule"
    ]

def _transitive_source_input_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = _install_actions(env)
    asserts.equals(env, 1, len(actions))
    action = actions[0]
    inputs = action.inputs.to_list()
    input_basenames = [file.basename for file in inputs]
    input_paths = [file.path for file in inputs]

    asserts.true(
        env,
        ctx.attr.expected_source in input_basenames,
        "a parent install action must declare original files named by child manifests",
    )
    asserts.true(
        env,
        ctx.attr.expected_generated_source in input_basenames,
        "generated child test lists must propagate as original transitive sources",
    )
    unexpected_child_outputs = [
        path
        for path in input_paths
        if "/%s/" % ctx.attr.child_install_dir in path
    ]
    asserts.equals(
        env,
        [],
        unexpected_child_outputs,
        "a parent install action must not consume child install outputs",
    )
    asserts.false(
        env,
        ctx.attr.child_install_dir in input_basenames,
        "a parent install action must not consume a child install depfile",
    )
    action_output_basenames = [file.basename for file in action.outputs.to_list()]
    asserts.true(
        env,
        ctx.attr.expected_renamed_output in action_output_basenames,
        "a parent install action must preserve a child's include_files destination",
    )
    asserts.false(
        env,
        ctx.attr.original_include_basename in action_output_basenames,
        "a transitive include must not be reclassified under its original basename",
    )

    argv = action.argv if action.argv != None else []
    depfile_args = [arg for arg in argv if arg.startswith("--depfile=")]
    asserts.equals(env, 1, len(depfile_args), "a parent action must use only its flattened depfile")

    target = analysistest.target_under_test(env)
    source_map = json.decode(target[MongoInstallInfo].src_map.to_list()[0])
    asserts.true(env, ctx.attr.expected_renamed_path in source_map["includes"].values())
    transitive_source_basenames = [
        file.basename
        for file in target[MongoInstallInfo].source_files.to_list()
    ]
    asserts.true(env, ctx.attr.expected_generated_source in transitive_source_basenames)
    return analysistest.end(env)

_transitive_source_input_test = analysistest.make(
    _transitive_source_input_test_impl,
    attrs = {
        "child_install_dir": attr.string(mandatory = True),
        "expected_generated_source": attr.string(mandatory = True),
        "expected_renamed_output": attr.string(mandatory = True),
        "expected_renamed_path": attr.string(mandatory = True),
        "expected_source": attr.string(mandatory = True),
        "original_include_basename": attr.string(mandatory = True),
    },
)

def _diamond_deduplication_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = _install_actions(env)
    asserts.equals(env, 1, len(actions))
    action = actions[0]
    asserts.equals(
        env,
        1,
        len(action.outputs.to_list()),
        "an identical artifact arriving through a diamond must have one declared output",
    )

    target = analysistest.target_under_test(env)
    install_owners = target[MongoInstallInfo].install_owners
    asserts.equals(env, 1, len(install_owners))
    owner = install_owners.values()[0]
    asserts.true(env, owner.source.endswith("/" + ctx.attr.expected_source_basename))
    asserts.true(env, owner.owner.endswith(ctx.attr.expected_owner_suffix))
    return analysistest.end(env)

_diamond_deduplication_test = analysistest.make(
    _diamond_deduplication_test_impl,
    attrs = {
        "expected_owner_suffix": attr.string(mandatory = True),
        "expected_source_basename": attr.string(mandatory = True),
    },
)

def _multi_destination_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = _install_actions(env)
    asserts.equals(env, 1, len(actions))
    asserts.equals(env, 2, len(actions[0].outputs.to_list()))

    target = analysistest.target_under_test(env)
    destinations = sorted([
        owner.destination
        for owner in target[MongoInstallInfo].install_owners.values()
    ])
    asserts.equals(env, ["bin/multi.exe", "share/multi.exe"], destinations)
    return analysistest.end(env)

_multi_destination_test = analysistest.make(_multi_destination_test_impl)

def _test_data_installation_test_impl(ctx):
    env = analysistest.begin(ctx)
    actions = _install_actions(env)
    asserts.equals(env, 1, len(actions))

    target = analysistest.target_under_test(env)
    package_files = target[PackageFilesInfo].dest_src_map
    asserts.true(
        env,
        ctx.attr.expected_destination in package_files,
        "test data must be present in the installed runfiles layout",
    )

    input_basenames = [file.basename for file in actions[0].inputs.to_list()]
    asserts.true(
        env,
        ctx.attr.expected_data_basename in input_basenames,
        "test data must be an input to the install action",
    )

    source_basenames = [file.basename for file in target[MongoInstallInfo].source_files.to_list()]
    asserts.true(env, ctx.attr.expected_data_basename in source_basenames)
    return analysistest.end(env)

_test_data_installation_test = analysistest.make(
    _test_data_installation_test_impl,
    attrs = {
        "expected_data_basename": attr.string(mandatory = True),
        "expected_destination": attr.string(mandatory = True),
    },
)

def _expected_failure_test_impl(ctx):
    env = analysistest.begin(ctx)
    asserts.expect_failure(env, ctx.attr.expected_failure)
    return analysistest.end(env)

_expected_failure_test = analysistest.make(
    _expected_failure_test_impl,
    expect_failure = True,
    attrs = {
        "expected_failure": attr.string(mandatory = True),
    },
)

_GDB_TOOLCHAIN_DESTINATION = "lib/gdb-toolchain"

def _gdb_root_files_impl(ctx):
    env = analysistest.begin(ctx)
    package_files = analysistest.target_under_test(env)[PackageFilesInfo].dest_src_map
    asserts.equals(
        env,
        ctx.attr.expected_gdb,
        _GDB_TOOLCHAIN_DESTINATION in package_files,
        "GDB contents did not match install_gdb",
    )
    return analysistest.end(env)

def _gdb_install_root_files_impl(ctx):
    env = analysistest.begin(ctx)
    package_files = analysistest.target_under_test(env)[PackageFilesInfo].dest_src_map
    asserts.true(
        env,
        _GDB_TOOLCHAIN_DESTINATION in package_files,
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
    input_source = name + "_input_source"
    input_include_source = name + "_input_include_source"
    input_leaf = name + "_input_leaf"
    input_root = name + "_input_root"
    _generated_file(
        name = input_include_source,
        output = "test_inputs/original-include-name.so",
        testonly = True,
    )
    _generated_file(
        name = input_source,
        output = "test_inputs/%s.exe" % input_source,
        test_binary = True,
        testonly = True,
    )
    _install_rule(
        name = input_leaf,
        include_files = {":" + input_include_source: "lib/renamed-include.so"},
        srcs = [":" + input_source],
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
    _install_rule(
        name = input_root,
        deps = [":" + input_leaf],
    )
    input_test = name + "_transitive_inputs_test"
    _transitive_source_input_test(
        name = input_test,
        target_under_test = ":" + input_root,
        child_install_dir = input_leaf,
        expected_source = input_source + ".exe",
        expected_generated_source = input_leaf + "_test_list.txt",
        expected_renamed_output = "renamed-include.so",
        expected_renamed_path = "lib/renamed-include.so",
        original_include_basename = "original-include-name.so",
    )

    exact_left = name + "_exact_left"
    exact_right = name + "_exact_right"
    _generated_file(
        name = exact_left,
        output = "test_inputs/exact_left/collision.exe",
        testonly = True,
    )
    _generated_file(
        name = exact_right,
        output = "test_inputs/exact_right/collision.exe",
        testonly = True,
    )
    exact_target = name + "_exact_collision_target"
    _install_rule(
        name = exact_target,
        srcs = [":" + exact_left, ":" + exact_right],
        tags = ["manual"],
    )
    exact_test = name + "_exact_collision_test"
    _expected_failure_test(
        name = exact_test,
        target_under_test = ":" + exact_target,
        expected_failure = "install destination collision",
    )

    prefix_directory = name + "_prefix_directory"
    prefix_file = name + "_prefix_file"
    _generated_directory(
        name = prefix_directory,
        output = "test_inputs/prefix/tree",
        testonly = True,
    )
    _generated_file(
        name = prefix_file,
        output = "test_inputs/prefix_child/child",
        testonly = True,
    )
    prefix_target = name + "_prefix_collision_target"
    _install_rule(
        name = prefix_target,
        root_files = {":" + prefix_directory: "lib"},
        include_files = {":" + prefix_file: "lib/tree/child"},
        tags = ["manual"],
    )
    prefix_test = name + "_prefix_collision_test"
    _expected_failure_test(
        name = prefix_test,
        target_under_test = ":" + prefix_target,
        expected_failure = "install destination prefix collision",
    )

    invalid_source = name + "_invalid_source"
    _generated_file(
        name = invalid_source,
        output = "test_inputs/invalid/source",
        testonly = True,
    )
    invalid_target = name + "_invalid_path_target"
    _install_rule(
        name = invalid_target,
        include_files = {":" + invalid_source: "lib/../escape"},
        tags = ["manual"],
    )
    invalid_test = name + "_invalid_path_test"
    _expected_failure_test(
        name = invalid_test,
        target_under_test = ":" + invalid_target,
        expected_failure = "invalid install destination",
    )

    diamond_source = name + "_diamond_source"
    diamond_leaf = name + "_diamond_leaf"
    diamond_left = name + "_diamond_left"
    diamond_right = name + "_diamond_right"
    diamond_root = name + "_diamond_root"
    diamond_source_path = "test_inputs/diamond/diamond.exe"
    _generated_file(
        name = diamond_source,
        output = diamond_source_path,
        testonly = True,
    )
    _install_rule(
        name = diamond_leaf,
        srcs = [":" + diamond_source],
    )
    _install_rule(
        name = diamond_left,
        deps = [":" + diamond_leaf],
    )
    _install_rule(
        name = diamond_right,
        deps = [":" + diamond_leaf],
    )
    _install_rule(
        name = diamond_root,
        deps = [":" + diamond_left, ":" + diamond_right],
    )
    diamond_test = name + "_diamond_deduplication_test"
    _diamond_deduplication_test(
        name = diamond_test,
        target_under_test = ":" + diamond_root,
        expected_owner_suffix = "//bazel/install_rules:%s" % diamond_root,
        expected_source_basename = "diamond.exe",
    )

    multi_source = name + "_multi_destination_source"
    multi_target = name + "_multi_destination_target"
    _generated_file(
        name = multi_source,
        output = "test_inputs/multi/multi.exe",
        testonly = True,
    )
    _install_rule(
        name = multi_target,
        root_files = {":" + multi_source: "share"},
        srcs = [":" + multi_source],
    )
    multi_test = name + "_multi_destination_test"
    _multi_destination_test(
        name = multi_test,
        target_under_test = ":" + multi_target,
    )

    runfiles_data = name + "_runfiles_data"
    _generated_file(
        name = runfiles_data,
        output = "test_inputs/runfiles_data.txt",
        testonly = True,
    )
    runfiles_test_input = name + "_runfiles_test_input"
    _generated_test_with_data(
        name = runfiles_test_input,
        output = "test_inputs/runfiles_test.exe",
        data = [":" + runfiles_data],
        testonly = True,
    )
    runfiles_install = name + "_runfiles_install"
    _install_rule(
        name = runfiles_install,
        srcs = [":" + runfiles_test_input],
    )
    runfiles_test = name + "_test_data_installation_test"
    _test_data_installation_test(
        name = runfiles_test,
        target_under_test = ":" + runfiles_install,
        expected_data_basename = "runfiles_data.txt",
        expected_destination = "bin/_main/bazel/install_rules/test_inputs/runfiles_data.txt",
    )

    native.test_suite(
        name = name,
        tests = [
            ":" + input_test,
            ":" + exact_test,
            ":" + prefix_test,
            ":" + invalid_test,
            ":" + diamond_test,
            ":" + multi_test,
            ":" + runfiles_test,
        ],
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
