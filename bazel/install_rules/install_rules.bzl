"""
Sets up install and archive rules.
"""

load("@rules_pkg//:pkg.bzl", "pkg_tar")
load("@rules_pkg//pkg:providers.bzl", "PackageFilesInfo")
load("@bazel_skylib//lib:paths.bzl", "paths")
load("//bazel:mongo_src_rules.bzl", "SANITIZER_DATA", "SANITIZER_ENV")
load("//bazel:separate_debug.bzl", "TagInfo")
load("//bazel/install_rules:pretty_printer_tests.bzl", "mongo_pretty_printer_test")
load("//bazel/install_rules:providers.bzl", "TestBinaryInfo")

MongoInstallInfo = provider(
    doc = "A install rule provider to pass around deps files",
    fields = {
        "deps_files": "Install rule file describing the files installed for passing to script",
        "test_file": "File containing list of installed tests",
        "src_map": "contents of the dep file for use in rules",
    },
)

# This is a dictionary because there are no sets in bazel
# and we want to look up if the value exists quickly
TEST_TAGS = {
    "bsoncolumn_bm": 1,
    "mongo_benchmark": 1,
    "mongo_fuzzer_test": 1,
    "mongo_integration_test": 1,
    "mongo_unittest": 1,
    "mongo_unittest_first_group": 1,
    "mongo_unittest_second_group": 1,
    "mongo_unittest_third_group": 1,
    "mongo_unittest_fourth_group": 1,
    "mongo_unittest_fifth_group": 1,
    "mongo_unittest_sixth_group": 1,
    "mongo_unittest_seventh_group": 1,
    "mongo_unittest_eighth_group": 1,
    "query_bm": 1,
    "repl_bm": 1,
    "sharding_bm": 1,
    "sep_bm": 1,
    "storage_bm": 1,
}

def test_binary_aspect_impl(target, ctx):
    """Collect all test binaries from transitive srcs and deps

    Args:
        target: current target
        ctx: context of current target

    Returns:
        struct containing collected test binaries
    """
    transitive_deps = []

    if TestBinaryInfo in target:
        return []

    if TagInfo in target:
        for tag in target[TagInfo].tags:
            if tag in TEST_TAGS:
                transitive_deps.append(target.files)
                break

    if hasattr(ctx.rule.attr, "srcs"):
        for src in ctx.rule.attr.srcs:
            if TestBinaryInfo in src:
                transitive_deps.append(src[TestBinaryInfo].test_binaries)

    if hasattr(ctx.rule.attr, "deps"):
        for dep in ctx.rule.attr.deps:
            if TestBinaryInfo in dep:
                transitive_deps.append(dep[TestBinaryInfo].test_binaries)

    test_binaries = depset(transitive = transitive_deps)
    return [TestBinaryInfo(test_binaries = test_binaries)]

test_binary_aspect = aspect(
    implementation = test_binary_aspect_impl,
    attr_aspects = ["srcs", "deps"],
)

def get_constraints(ctx):
    """Return rule time constaints.

    Args:
        ctx: rule ctx

    Returns:
        3 element tuple with each OS constraint.
    """
    linux_constraint = ctx.attr._linux_constraint[platform_common.ConstraintValueInfo]
    macos_constraint = ctx.attr._macos_constraint[platform_common.ConstraintValueInfo]
    windows_constraint = ctx.attr._windows_constraint[platform_common.ConstraintValueInfo]
    return linux_constraint, macos_constraint, windows_constraint

def is_binary_file(ctx, basename):
    """Check if file looks like a binary

    Args:
        ctx: rule ctx
        file: file to check

    Returns:
        True if it looks like a binary, False otherwise
    """
    linux_constraint, macos_constraint, windows_constraint = get_constraints(ctx)
    if ctx.target_platform_has_constraint(linux_constraint):
        return not basename.startswith("lib")
    elif ctx.target_platform_has_constraint(macos_constraint):
        return not basename.startswith("lib")
    elif ctx.target_platform_has_constraint(windows_constraint):
        return basename.endswith(".exe") or basename.endswith(".pdb") or basename.endswith(".dll")
    else:
        ctx.fail("Unknown OS")
        return False

def is_debug_file(ctx, basename):
    """Check if file looks a debug file

    Args:
        ctx: rule ctx
        file: file to check

    Returns:
        True if it looks like a debug file, False otherwise
    """
    linux_constraint, macos_constraint, windows_constraint = get_constraints(ctx)
    if ctx.target_platform_has_constraint(linux_constraint):
        return basename.endswith(".debug")
    elif ctx.target_platform_has_constraint(macos_constraint):
        return basename.endswith(".dSYM")
    elif ctx.target_platform_has_constraint(windows_constraint):
        return basename.endswith(".pdb")
    else:
        ctx.fail("Unknown OS")
        return False

def declare_output(ctx, output, is_directory):
    """Declare an output as either a file or directory

    Args:
        ctx: rule ctx
        output: output file to declare
        is_directory: determines if the file is a directory

    Returns:
        File object representing output
    """
    if is_directory:
        return ctx.actions.declare_directory(output)
    else:
        return ctx.actions.declare_file(output)

def sort_file(ctx, file, install_dir, file_map, is_directory):
    """Determine location a file should be installed to

    Args:
        ctx: rule ctx
        file: file to sort
        install_dir: the directory to install to
        file_map: dict containing specific file designations
        is_directory: determines if the file is a directory

    """
    _, macos_constraint, _ = get_constraints(ctx)
    basename = paths.basename(file)
    bin_install = install_dir + "/bin/" + basename
    lib_install = install_dir + "/lib/" + basename

    if is_binary_file(ctx, basename) or basename.endswith(".py"):
        if not is_debug_file(ctx, basename):
            if ctx.attr.debug != "debug":
                file_map["binaries"][file] = declare_output(ctx, bin_install, is_directory)
        elif ctx.attr.debug != "stripped":
            file_map["binaries_debug"][file] = declare_output(ctx, bin_install, is_directory)

    elif not is_debug_file(ctx, basename):
        if ctx.attr.debug != "debug":
            file_map["dynamic_libs"][file] = declare_output(ctx, lib_install, is_directory)

    elif ctx.attr.debug != "stripped":
        file_map["dynamic_libs_debug"][file] = declare_output(ctx, lib_install, is_directory)

def mongo_install_rule_impl(ctx):
    """Perform install actions

    Args:
        ctx: rule ctx

    Returns:
        DefaultInfo: with dep files and output file
        PackageFilesInfo: with a mapping for creating the archive
    """
    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime

    file_map = {
        "binaries": {},
        "binaries_debug": {},
        "dynamic_libs_debug": {},
        "dynamic_libs": {},
    }
    test_files = []
    outputs = []
    install_dir = ctx.label.name

    # sort direct sources
    for input_bin in ctx.attr.srcs:
        test_files.extend(input_bin[TestBinaryInfo].test_binaries.to_list())
        for bin in input_bin.files.to_list():
            sort_file(ctx, bin.path, install_dir, file_map, bin.is_directory)

    # sort dependency install files
    for dep in ctx.attr.deps:
        test_files.extend(dep[TestBinaryInfo].test_binaries.to_list())

        # Create a map of filename to if its a directory, ie. { coolfolder: True, coolfile: False } as the json loses that info
        file_directory_map = {file_dep.basename: file_dep.is_directory for file_dep in dep[DefaultInfo].files.to_list()}
        src_map = json.decode(dep[MongoInstallInfo].src_map.to_list()[0])
        files = []
        for key in src_map:
            files.extend(src_map[key])
        for file in files:
            sort_file(ctx, file, install_dir, file_map, file_directory_map[file.split("/")[-1]])

    # aggregate based on type of installs
    if ctx.attr.debug == "stripped":
        bins = [bin for bin in file_map["binaries"]]
        libs = [lib for lib in file_map["dynamic_libs"]]
    elif ctx.attr.debug == "debug":
        bins = [bin for bin in file_map["binaries_debug"]]
        libs = [lib for lib in file_map["dynamic_libs_debug"]]
    else:
        bins = [bin for bin in file_map["binaries"]] + [bin for bin in file_map["binaries_debug"]]
        libs = [lib for lib in file_map["dynamic_libs"]] + [lib for lib in file_map["dynamic_libs_debug"]]

    unittest_bin = None
    if len(bins) == 1 and ctx.attr.debug != "debug" and file_map["binaries"][bins[0]].basename.endswith("_test"):
        unittest_bin = file_map["binaries"][bins[0]]

    # Write installed_tests.txt which contains the list of all test files installed
    input_deps = []
    installed_tests = []
    for file in test_files:
        if not is_debug_file(ctx, file.basename) and ctx.attr.debug != "debug":
            installed_tests.append(file_map["binaries"][file.path].path)

    installed_test_list_file = None
    if len(installed_tests) > 0:
        installed_test_list_file = ctx.actions.declare_file("install_deps/" + install_dir + "_test_list.txt")
        ctx.actions.write(
            output = installed_test_list_file,
            content = "\n".join(installed_tests),
        )
        input_deps.append(installed_test_list_file)
        real_test_list_output_location = ctx.actions.declare_file(install_dir + "/" + installed_test_list_file.basename)
        outputs.append(real_test_list_output_location)

    # create a dep file for passing all the files we intend to install
    # to the python script
    name = ctx.label.package + "_" + install_dir
    name = name.replace("/", "_")
    deps_file = ctx.actions.declare_file("install_deps/" + name + "/" + install_dir)
    json_out = struct(
        roots = [] if installed_test_list_file == None else [installed_test_list_file.path],
        bins = bins,
        libs = libs,
    )
    ctx.actions.write(
        output = deps_file,
        content = json_out.to_json(),
    )

    # create a mapping of source location to install location
    pkg_dict = {}
    flat_map = {}

    for file_type in file_map:
        flat_map |= file_map[file_type]
    for file in bins:
        pkg_dict["bin/" + flat_map[file].basename] = flat_map[file]
        outputs.append(flat_map[file])
    for file in libs:
        pkg_dict["lib/" + flat_map[file].basename] = flat_map[file]
        outputs.append(flat_map[file])

    # resolve full install dir for python script input
    full_install_dir = ctx.bin_dir.path
    if ctx.label.package:
        full_install_dir += "/" + ctx.label.package
    full_install_dir += "/" + install_dir

    input_deps.append(deps_file)

    inputs = depset(direct = input_deps, transitive = [
        ctx.attr._install_script.files,
        python.files,
    ] + [f.files for f in ctx.attr.srcs] + [dep[MongoInstallInfo].deps_files for dep in ctx.attr.deps] + [dep[DefaultInfo].files for dep in ctx.attr.deps])

    if outputs:
        ctx.actions.run(
            executable = python.interpreter.path,
            outputs = outputs,
            inputs = inputs,
            arguments = [
                ctx.attr._install_script.files.to_list()[0].path,
                "--depfile=" + deps_file.path,
                "--install-dir=" + full_install_dir,
            ] + ["--depfile=" + str(dep[MongoInstallInfo].deps_files.to_list()[0].path) for dep in ctx.attr.deps],
            mnemonic = "MongoInstallRule",
            execution_requirements = {
                "no-cache": "1",
                "no-sandbox": "1",
                "no-remote": "1",
                "local": "1",
            },
        )

    runfiles = ctx.runfiles(files = outputs)
    for input_bin in ctx.attr.srcs:
        runfiles = runfiles.merge(input_bin[DefaultInfo].data_runfiles)

    if unittest_bin:
        outputs = depset([unittest_bin])
    else:
        outputs = depset(outputs)

    return [
        DefaultInfo(
            files = outputs,
            executable = unittest_bin,
            runfiles = runfiles,
        ),
        PackageFilesInfo(
            dest_src_map = pkg_dict,
        ),
        MongoInstallInfo(
            deps_files = depset([deps_file], transitive = [dep[MongoInstallInfo].deps_files for dep in ctx.attr.deps]),
            test_file = installed_test_list_file,
            src_map = depset([json_out.to_json()]),
        ),
    ]

mongo_install_rule = rule(
    mongo_install_rule_impl,
    attrs = {
        "srcs": attr.label_list(aspects = [test_binary_aspect]),
        "deps": attr.label_list(providers = [PackageFilesInfo], aspects = [test_binary_aspect]),
        "debug": attr.string(),
        "_install_script": attr.label(allow_single_file = True, default = "//bazel/install_rules:install_rules.py"),
        "_linux_constraint": attr.label(default = "@platforms//os:linux"),
        "_macos_constraint": attr.label(default = "@platforms//os:macos"),
        "_windows_constraint": attr.label(default = "@platforms//os:windows"),
    },
    doc = "Install targets",
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
)

def mongo_install(
        name,
        srcs,
        deps = [],
        target_compatible_with = [],
        testonly = False,
        pretty_printer_tests = {},
        **kwargs):
    """Perform install actions

    Args:
        name: standard target name
        srcs: list of targets that should be installed
        deps: other install rule targets that should be installed
        **kwargs: other args to pass to underlying rules
        target_compatible_with: forward target_compatible_with args to the rules

    """
    compressor = select({
        "@pigz//:pigz_tool_available": "@pigz//:bin",
        "//conditions:default": None,
    })

    ext = select({
        "@platforms//os:linux": ".tar.gz",
        "@platforms//os:windows": ".zip",
        "@platforms//os:macos": ".tar.gz",
    })

    # this macro create several install targets for each instance of an install:
    # "": normal install includes bins and debug info
    # stripped: only install bins, only available with separate_debug=True
    # debug: only install debug, only available with separate_debug=True
    for install_type in ["", "-stripped", "-debug"]:
        modified_srcs = srcs
        install_target = "install-" + name + install_type
        debug = ""
        if install_type:
            debug = install_type[1:]

        # The macro names are base names, where install and archive are prefixed.
        # this means for deps of install rule types we need to append a prefix
        # and this means that the deps feild is restricted from selects.
        #
        # if a select is needed it can be applied at the srcs level
        #
        # however we add a special case this for enteprise install packages
        dep_targets = []
        community_dep_targets = []
        for dep in deps:
            if ":" in dep:
                dep_basename = dep.split(":")[1]
                dep_package = dep.split(":")[0]
                if "modules/enterprise" not in native.package_name() and "modules/enterprise" not in dep_package:
                    community_dep_targets.append(dep_package + ":install-" + dep_basename)
                dep_targets.append(dep_package + ":install-" + dep_basename)
            else:
                if "modules/enterprise" not in native.package_name():
                    community_dep_targets.append("install-" + dep)
                dep_targets.append("install-" + dep)

        # separate debug is required to make stripped or debug packages
        seperate_debug_incompat = []
        if install_type:
            seperate_debug_incompat = ["@platforms//:incompatible"]

        if len(pretty_printer_tests) > 0 and install_type != "-debug":
            for test_script, test_binary in pretty_printer_tests.items():
                pretty_printer_name = install_target + "-" + test_script.split(":")[-1].split(".")[0]
                mongo_pretty_printer_test(
                    name = pretty_printer_name,
                    test_script = test_script,
                    test_binary = test_binary,
                    testonly = True,
                )
                modified_srcs = modified_srcs + [pretty_printer_name]
            testonly = True

        mongo_install_rule(
            name = install_target,
            srcs = modified_srcs,
            debug = debug,
            deps = select({
                "//bazel/config:build_enterprise_enabled": dep_targets,
                "//conditions:default": community_dep_targets,
            }),
            target_compatible_with = target_compatible_with + select({
                "//bazel/config:separate_debug_enabled": [],
                "//conditions:default": seperate_debug_incompat,
            }),
            testonly = testonly,
            **kwargs
        )

        # package up the the install into an archive.
        pkg_tar(
            name = "archive-" + name + install_type,
            srcs = [install_target],
            compressor = compressor,
            package_file_name = name + install_type + ext,
            exec_properties = {
                "no-cache": "1",
                "no-sandbox": "1",
                "no-remote": "1",
                "local": "1",
            },
            testonly = testonly,
            **kwargs
        )
