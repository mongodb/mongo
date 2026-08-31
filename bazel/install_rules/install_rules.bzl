"""
Sets up install and archive rules.
"""

load("@rules_pkg//:pkg.bzl", "pkg_tar", "pkg_zip")
load("@rules_pkg//:mappings.bzl", "pkg_attributes", "pkg_files")
load("@rules_pkg//pkg:providers.bzl", "PackageFilesInfo")
load("@bazel_skylib//lib:paths.bzl", "paths")
load("//bazel:mongo_src_rules.bzl", "SANITIZER_DATA", "SANITIZER_ENV")
load("//bazel:separate_debug.bzl", "TagInfo")
load("//bazel/install_rules:pretty_printer_tests.bzl", "mongo_pretty_printer_test")
load("//bazel/install_rules:providers.bzl", "TestBinaryInfo")
load("//bazel/toolchains/cc:mongo_errors.bzl", "DWP_ERROR_MESSAGE")
load("//bazel:transitions.bzl", "extensions_transition")
load("@rules_cc//cc/common:debug_package_info.bzl", "DebugPackageInfo")

_WINDOWS_BINARY_EXTENSIONS = {
    ".dll": True,
    ".exe": True,
    ".pdb": True,
    ".ps1": True,
}

_WINDOWS_DEBUG_EXTENSIONS = {
    ".pdb": True,
}

# Windows reserves these DOS device names, even when they appear with a file extension.
_WINDOWS_RESERVED_BASENAMES = {
    "AUX": True,
    "COM1": True,
    "COM2": True,
    "COM3": True,
    "COM4": True,
    "COM5": True,
    "COM6": True,
    "COM7": True,
    "COM8": True,
    "COM9": True,
    "CON": True,
    "LPT1": True,
    "LPT2": True,
    "LPT3": True,
    "LPT4": True,
    "LPT5": True,
    "LPT6": True,
    "LPT7": True,
    "LPT8": True,
    "LPT9": True,
    "NUL": True,
    "PRN": True,
}

_LINUX_DEBUG_EXTENSIONS = {
    ".debug": True,
    ".dwp": True,
}

_MACOS_DEBUG_EXTENSIONS = {
    ".dSYM": True,
}

# Used to skip rules on certain OS architectures
def _empty_rule_impl(ctx):
    pass

empty_rule = rule(
    implementation = _empty_rule_impl,
)

MongoInstallInfo = provider(
    doc = "A install rule provider to pass around deps files",
    fields = {
        "deps_files": "Install rule file describing the files installed for passing to script",
        "test_file": "File containing list of installed tests",
        "src_map": "contents of the dep file for use in rules",
        "source_files": "Original source files referenced by transitive install depfiles",
        "install_owners": "Normalized install destinations and their owning source artifacts",
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
    "first_half_bm": 1,
    "second_half_bm": 1,
}

def test_binary_aspect_impl(target, ctx):
    """Collect all test binaries and their data files from transitive srcs and deps

    Args:
        target: current target
        ctx: context of current target

    Returns:
        provider containing collected test binaries and test data files
    """
    transitive_test_binaries = []
    transitive_test_data = []

    if TestBinaryInfo in target:
        return []

    if TagInfo in target:
        for tag in target[TagInfo].tags:
            if tag in TEST_TAGS:
                transitive_test_binaries.append(target.files)
                transitive_test_data.append(target[DefaultInfo].data_runfiles.files)
                break

    if hasattr(ctx.rule.attr, "srcs"):
        for src in ctx.rule.attr.srcs:
            if TestBinaryInfo in src:
                transitive_test_binaries.append(src[TestBinaryInfo].test_binaries)
                if hasattr(src[TestBinaryInfo], "test_data"):
                    transitive_test_data.append(src[TestBinaryInfo].test_data)

    if hasattr(ctx.rule.attr, "deps"):
        for dep in ctx.rule.attr.deps:
            if TestBinaryInfo in dep:
                transitive_test_binaries.append(dep[TestBinaryInfo].test_binaries)
                if hasattr(dep[TestBinaryInfo], "test_data"):
                    transitive_test_data.append(dep[TestBinaryInfo].test_data)

    test_binaries = depset(transitive = transitive_test_binaries)
    test_data = depset(transitive = transitive_test_data)
    return [TestBinaryInfo(test_binaries = test_binaries, test_data = test_data)]

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

def _platform_kind(ctx):
    linux_constraint, macos_constraint, windows_constraint = get_constraints(ctx)
    if ctx.target_platform_has_constraint(linux_constraint):
        return "linux"
    if ctx.target_platform_has_constraint(macos_constraint):
        return "macos"
    if ctx.target_platform_has_constraint(windows_constraint):
        return "windows"
    ctx.fail("Unknown OS")
    return ""

def _basename(path):
    slash = path.rfind("/")
    if slash == -1:
        return path
    return path[slash + 1:]

def _extension(basename):
    dot = basename.rfind(".")
    if dot == -1:
        return ""
    return basename[dot:]

def is_binary_file(platform_kind, basename):
    """Check if file looks like a binary

    Args:
        ctx: rule ctx
        file: file to check

    Returns:
        True if it looks like a binary, False otherwise
    """
    if platform_kind == "linux":
        return not (basename.startswith("lib") or basename.startswith("mongo_crypt_v") or basename.startswith("stitch_support.so"))
    elif platform_kind == "macos":
        return not (basename.startswith("lib") or basename.startswith("mongo_crypt_v") or basename.startswith("stitch_support.dylib"))
    elif platform_kind == "windows":
        return _extension(basename) in _WINDOWS_BINARY_EXTENSIONS
    else:
        return False

def is_debug_file(platform_kind, basename):
    """Check if file looks a debug file

    Args:
        ctx: rule ctx
        file: file to check

    Returns:
        True if it looks like a debug file, False otherwise
    """
    ext = _extension(basename)
    if platform_kind == "linux":
        return ext in _LINUX_DEBUG_EXTENSIONS
    elif platform_kind == "macos":
        return ext in _MACOS_DEBUG_EXTENSIONS
    elif platform_kind == "windows":
        return ext in _WINDOWS_DEBUG_EXTENSIONS
    else:
        return False

def _destination_in_directory(directory, basename):
    if not directory:
        return basename
    return directory + "/" + basename

def _test_data_files(test_binary_info):
    """Return test data from TestBinaryInfo, including legacy provider instances."""
    if hasattr(test_binary_info, "test_data"):
        return test_binary_info.test_data.to_list()
    return []

def _runfiles_install_destination(file):
    """Return the install path that matches the file's Bazel runfiles path.

    The C++ runfiles library addresses files in the main repository below `_main`. External
    repository paths are represented by File.short_path as `../<repo>/...`, but are addressed
    without the `../` prefix by Rlocation.
    """
    runfiles_path = file.short_path
    if runfiles_path.startswith("../"):
        runfiles_path = runfiles_path[3:]
    elif not runfiles_path.startswith("_main/"):
        runfiles_path = "_main/" + runfiles_path
    return "bin/" + runfiles_path

def _normalize_install_destination(ctx, destination, platform_kind):
    """Validate and normalize a path relative to an install tree."""
    if not destination:
        fail("invalid install destination for %s: path is empty" % ctx.label)
    if "\\" in destination:
        fail("invalid install destination for %s: backslashes are not allowed in '%s'" % (ctx.label, destination))
    if paths.is_absolute(destination) or ":" in destination:
        fail("invalid install destination for %s: absolute path '%s'" % (ctx.label, destination))

    for component in destination.split("/"):
        if not component or component == "." or component == "..":
            fail("invalid install destination for %s: invalid component in '%s'" % (ctx.label, destination))
        if platform_kind == "windows":
            if component.endswith(".") or component.endswith(" "):
                fail("invalid Windows install destination for %s: '%s'" % (ctx.label, destination))
            if component.split(".")[0].upper() in _WINDOWS_RESERVED_BASENAMES:
                fail("reserved Windows install destination for %s: '%s'" % (ctx.label, destination))

    normalized = paths.normalize(destination)
    if normalized == "." or normalized.startswith("../"):
        fail("invalid install destination for %s: path escapes the install tree: '%s'" % (ctx.label, destination))

    # Install trees and archives move between filesystems. Conservatively reject case-only
    # aliases even when the current execution filesystem happens to be case-sensitive.
    return normalized, normalized.lower()

def _record_file_map_output(ctx, file_map, category, source, output):
    """Record a source-keyed manifest entry without silently losing a second destination."""
    existing = file_map[category].get(source)
    if existing != None and existing.path != output.path:
        fail(
            "install source '%s' has multiple %s destinations in %s: '%s' and '%s'" % (
                source,
                category,
                ctx.label,
                existing.short_path,
                output.short_path,
            ),
        )
    file_map[category][source] = output

def _declare_install_output(
        ctx,
        install_dir,
        destination,
        source,
        is_directory,
        platform_kind,
        install_owners,
        owned_descendants):
    """Declare one uniquely owned artifact in an install tree.

    An identical source/destination/type may arrive through multiple dependency paths. It is
    represented by one output. Any other exact or ancestor overlap is ambiguous and rejected
    during analysis, before an install action can race while publishing the convenience tree.
    """
    normalized, destination_key = _normalize_install_destination(ctx, destination, platform_kind)
    owner = str(ctx.label)

    if destination_key in install_owners:
        existing = install_owners[destination_key]
        if existing.source == source and existing.is_directory == is_directory:
            return existing.output
        fail(
            ("install destination collision at '%s': %s owns source '%s' (%s), but %s " +
             "would install source '%s' (%s)") % (
                normalized,
                existing.owner,
                existing.source,
                "directory" if existing.is_directory else "file",
                owner,
                source,
                "directory" if is_directory else "file",
            ),
        )

    if destination_key in owned_descendants:
        descendant = install_owners[owned_descendants[destination_key]]
        fail(
            "install destination prefix collision: '%s' from %s is an ancestor of '%s' from %s" % (
                normalized,
                owner,
                descendant.destination,
                descendant.owner,
            ),
        )

    components = destination_key.split("/")
    prefix = ""
    for component in components[:-1]:
        prefix = component if not prefix else prefix + "/" + component
        if prefix in install_owners:
            ancestor = install_owners[prefix]
            fail(
                "install destination prefix collision: '%s' from %s is an ancestor of '%s' from %s" % (
                    ancestor.destination,
                    ancestor.owner,
                    normalized,
                    owner,
                ),
            )

    output_path = paths.join(install_dir, normalized)
    if is_directory:
        output = ctx.actions.declare_directory(output_path)
    else:
        output = ctx.actions.declare_file(output_path)

    install_owners[destination_key] = struct(
        destination = normalized,
        is_directory = is_directory,
        output = output,
        owner = owner,
        source = source,
    )
    prefix = ""
    for component in components[:-1]:
        prefix = component if not prefix else prefix + "/" + component
        if prefix not in owned_descendants:
            owned_descendants[prefix] = destination_key
    return output

def sort_file(
        ctx,
        file,
        basename,
        install_dir,
        file_map,
        is_directory,
        platform_kind,
        install_owners,
        owned_descendants):
    """Determine location a file should be installed to.

    Args:
        ctx: rule ctx
        file: file to sort
        install_dir: the directory to install to
        file_map: dict containing specific file designations
        is_directory: determines if the file is a directory

    """
    install_basename = basename
    ext = _extension(basename)
    if ext == ".dwp":
        # Due to us creating our binaries using the _with_debug name
        # the dwp files also contain it. Strip the _with_debug from the name
        install_basename = install_basename.replace("_with_debug.dwp", ".dwp")

    bin_install = "bin/" + install_basename
    lib_install = "lib/" + install_basename
    is_binary = is_binary_file(platform_kind, basename)
    is_debug = is_debug_file(platform_kind, basename)
    is_python = ext == ".py"

    if is_binary or is_python:
        if not is_debug:
            if ctx.attr.debug != "debug":
                file_map["binaries"][file] = _declare_install_output(ctx, install_dir, bin_install, file, is_directory, platform_kind, install_owners, owned_descendants)
        elif ctx.attr.debug != "stripped" or ctx.attr.publish_debug_in_stripped:
            file_map["binaries_debug"][file] = _declare_install_output(ctx, install_dir, bin_install, file, is_directory, platform_kind, install_owners, owned_descendants)

    elif not is_debug:
        if ctx.attr.debug != "debug":
            file_map["dynamic_libs"][file] = _declare_install_output(ctx, install_dir, lib_install, file, is_directory, platform_kind, install_owners, owned_descendants)

    elif ctx.attr.debug != "stripped" or ctx.attr.publish_debug_in_stripped:
        file_map["dynamic_libs_debug"][file] = _declare_install_output(ctx, install_dir, lib_install, file, is_directory, platform_kind, install_owners, owned_descendants)

def mongo_install_rule_impl(ctx):
    """Perform install actions

    Args:
        ctx: rule ctx

    Returns:
        DefaultInfo: with dep files and output file
        PackageFilesInfo: with a mapping for creating the archive
    """
    python = ctx.toolchains["@rules_python//python:toolchain_type"].py3_runtime

    file_map = {
        "binaries": {},
        "binaries_debug": {},
        "dynamic_libs_debug": {},
        "dynamic_libs": {},
        "root_files": {},
        "include_files": {},
    }
    test_files = []
    test_data_files = []
    outputs = []
    dwps = []
    install_owners = {}
    owned_descendants = {}
    install_dir = ctx.label.name
    platform_kind = _platform_kind(ctx)
    install_script = ctx.attr._install_script.files.to_list()[0]

    # sort direct sources
    for input_bin in ctx.attr.srcs:
        if DebugPackageInfo in input_bin and ctx.attr.create_dwp and ctx.attr.debug != "stripped":
            bin = input_bin[DebugPackageInfo].dwp_file
            dwps.append(bin)
            sort_file(ctx, bin.path, bin.basename, install_dir, file_map, bin.is_directory, platform_kind, install_owners, owned_descendants)
        input_test_binaries = input_bin[TestBinaryInfo].test_binaries.to_list()
        input_test_data = _test_data_files(input_bin[TestBinaryInfo])
        input_files = input_bin.files.to_list()
        test_files.extend(input_test_binaries)
        test_data_files.extend(input_test_data)
        for bin in input_files:
            sort_file(ctx, bin.path, bin.basename, install_dir, file_map, bin.is_directory, platform_kind, install_owners, owned_descendants)
        for data_file in input_test_data:
            destination = _runfiles_install_destination(data_file)
            output = _declare_install_output(
                ctx,
                install_dir,
                destination,
                data_file.path,
                data_file.is_directory,
                platform_kind,
                install_owners,
                owned_descendants,
            )
            _record_file_map_output(ctx, file_map, "root_files", data_file.path, output)

    for input_label, output_folder in ctx.attr.root_files.items():
        label_files = input_label.files.to_list()
        for file in label_files:
            destination = _destination_in_directory(output_folder, file.basename)
            output = _declare_install_output(ctx, install_dir, destination, file.path, file.is_directory, platform_kind, install_owners, owned_descendants)
            _record_file_map_output(ctx, file_map, "root_files", file.path, output)

    for input_label, output_path in ctx.attr.include_files.items():
        label_files = input_label.files.to_list()
        if len(label_files) != 1:
            fail("include_files label %s must produce exactly one file" % input_label.label)
        file = label_files[0]
        if file.is_directory:
            fail("include_files label %s must not produce a directory" % input_label.label)
        output = _declare_install_output(ctx, install_dir, output_path, file.path, False, platform_kind, install_owners, owned_descendants)
        _record_file_map_output(ctx, file_map, "include_files", file.path, output)

    # sort dependency install files
    for dep in ctx.attr.deps:
        dep_test_binaries = dep[TestBinaryInfo].test_binaries.to_list()
        dep_src_map_file = dep[MongoInstallInfo].src_map.to_list()[0]
        test_files.extend(dep_test_binaries)

        # The JSON source map intentionally stores paths, so retain directory information from
        # the original transitive inputs rather than forcing the dependency's install action.
        file_directory_map = {
            source.path: source.is_directory
            for source in dep[MongoInstallInfo].source_files.to_list()
        }
        src_map = json.decode(dep_src_map_file)
        for key in src_map:
            if key not in ["roots", "includes"]:
                for file in src_map[key]:
                    if file not in file_directory_map:
                        fail("install source '%s' from %s is missing from its transitive source files" % (file, dep.label))
                    filename = _basename(file)

                    # Due to us creating our binaries using the _with_debug name
                    # the dwp files also contain it. Strip the _with_debug from the name
                    filename = filename.replace("_with_debug.dwp", ".dwp")
                    sort_file(ctx, file, filename, install_dir, file_map, file_directory_map[file], platform_kind, install_owners, owned_descendants)
        for file, folder in src_map["roots"].items():
            if file not in file_directory_map:
                fail("install source '%s' from %s is missing from its transitive source files" % (file, dep.label))
            filename = _basename(file)
            destination = _destination_in_directory(folder, filename)
            output = _declare_install_output(ctx, install_dir, destination, file, file_directory_map[file], platform_kind, install_owners, owned_descendants)
            _record_file_map_output(ctx, file_map, "root_files", file, output)
        for file, output_path in src_map["includes"].items():
            if file not in file_directory_map:
                fail("install source '%s' from %s is missing from its transitive source files" % (file, dep.label))
            if file_directory_map[file]:
                fail("transitive include_files source '%s' from %s must not be a directory" % (file, dep.label))
            output = _declare_install_output(ctx, install_dir, output_path, file, False, platform_kind, install_owners, owned_descendants)
            _record_file_map_output(ctx, file_map, "include_files", file, output)

    # aggregate based on type of installs
    if ctx.attr.debug == "stripped" and not ctx.attr.publish_debug_in_stripped:
        bins = [bin for bin in file_map["binaries"]]
        libs = [lib for lib in file_map["dynamic_libs"]]
    elif ctx.attr.debug == "debug":
        bins = [bin for bin in file_map["binaries_debug"]]
        libs = [lib for lib in file_map["dynamic_libs_debug"]]
    else:
        bins = [bin for bin in file_map["binaries"]] + [bin for bin in file_map["binaries_debug"]]
        libs = [lib for lib in file_map["dynamic_libs"]] + [lib for lib in file_map["dynamic_libs_debug"]]
    root_files = [root_file for root_file in file_map["root_files"]]
    include_files = [include_file for include_file in file_map["include_files"]]

    unittest_bin = None
    if len(bins) == 1 and ctx.attr.debug != "debug" and file_map["binaries"][bins[0]].basename.endswith("_test") and len(root_files) == 0:
        unittest_bin = file_map["binaries"][bins[0]]

    # Write installed_tests.txt which contains the list of all test files installed
    input_deps = []
    installed_tests = []
    for file in test_files:
        file_basename = file.basename
        if not is_debug_file(platform_kind, file_basename) and ctx.attr.debug != "debug":
            if is_binary_file(platform_kind, file_basename) or _extension(file_basename) == ".py":
                test_path = file_map["binaries"][file.path].path

                # point at the binaries in bazel-bin/install/ rather than bazel-out/<some-arch>/bin/<some-install>/
                split_test_path = test_path.split("/")
                test_path = "bazel-bin/install/" + "/".join(split_test_path[4:])
                installed_tests.append(test_path)

    installed_test_list_file = None
    if len(installed_tests) > 0:
        installed_test_list_file = ctx.actions.declare_file("install_deps/" + install_dir + "_test_list.txt")
        ctx.actions.write(
            output = installed_test_list_file,
            content = "\n".join(installed_tests),
        )
        input_deps.append(installed_test_list_file)

    # create a dep file for passing all the files we intend to install
    # to the python script
    name = ctx.label.package + "_" + install_dir
    name = name.replace("/", "_")
    deps_file = ctx.actions.declare_file("install_deps/" + name + "/" + install_dir)

    destination_by_output_path = {
        owner.output.path: owner.destination
        for owner in install_owners.values()
    }

    # The roots are in the format { file : folder } so we can add arbitrary files to the install directory
    roots = {} if installed_test_list_file == None else {installed_test_list_file.path: ""}
    for file in root_files:
        output = file_map["root_files"][file]
        roots[file] = paths.dirname(destination_by_output_path[output.path])

    includes = {}
    for file in include_files:
        output = file_map["include_files"][file]
        includes[file] = destination_by_output_path[output.path]

    json_out = struct(
        roots = roots,
        includes = includes,
        bins = bins,
        libs = libs,
    )
    ctx.actions.write(
        output = deps_file,
        content = json.encode(json_out),
    )

    if len(installed_tests) > 0:
        real_test_list_output_location = _declare_install_output(
            ctx,
            install_dir,
            installed_test_list_file.basename,
            installed_test_list_file.path,
            False,
            platform_kind,
            install_owners,
            owned_descendants,
        )

    # A source may intentionally appear at multiple destinations or in multiple categories.
    # Build the declared outputs and package mapping from destination-keyed ownership so none
    # of those artifacts is lost through source-keyed flattening.
    pkg_dict = {}
    for owner in install_owners.values():
        pkg_dict[owner.destination] = owner.output
        outputs.append(owner.output)

    # resolve full install dir for python script input
    full_install_dir = ctx.bin_dir.path
    if ctx.label.package:
        full_install_dir += "/" + ctx.label.package
    full_install_dir += "/" + install_dir

    input_deps.append(deps_file)

    direct_source_files = test_files + test_data_files + dwps
    if installed_test_list_file != None:
        direct_source_files.append(installed_test_list_file)

    source_files = depset(direct = direct_source_files, transitive = [
        f.files
        for f in ctx.attr.srcs
    ] + [
        r.files
        for r in ctx.attr.root_files.keys()
    ] + [
        i.files
        for i in ctx.attr.include_files.keys()
    ] + [
        dep[MongoInstallInfo].source_files
        for dep in ctx.attr.deps
    ])

    inputs = depset(direct = input_deps, transitive = [
        ctx.attr._install_script.files,
        python.files,
        source_files,
    ])

    if outputs:
        ctx.actions.run(
            executable = python.interpreter.path,
            outputs = outputs,
            inputs = inputs,
            arguments = [
                install_script.path,
                "--depfile=" + deps_file.path,
                "--install-dir=" + full_install_dir,
            ],
            mnemonic = "MongoInstallRule",
            execution_requirements = {
                "no-cache": "1",
                # The install action publishes the shared bazel-bin/install convenience tree,
                # which is outside this action's declared outputs. It must not be cached or run
                # remotely. The wrapper selects the appropriate local or container strategy and
                # provides the writable shared-install root for publication.
                "no-remote": "1",
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
            install_owners = install_owners,
            test_file = installed_test_list_file,
            src_map = depset([json.encode(json_out)]),
            source_files = source_files,
        ),
    ]

mongo_install_rule = rule(
    mongo_install_rule_impl,
    attrs = {
        "srcs": attr.label_list(aspects = [test_binary_aspect]),
        "deps": attr.label_list(providers = [PackageFilesInfo], aspects = [test_binary_aspect]),
        "debug": attr.string(),
        "root_files": attr.label_keyed_string_dict(allow_files = True),
        "include_files": attr.label_keyed_string_dict(allow_files = True),
        "publish_debug_in_stripped": attr.bool(),
        "create_dwp": attr.bool(),
        "_install_script": attr.label(allow_single_file = True, default = "//bazel/install_rules:install_rules.py"),
        "_linux_constraint": attr.label(default = "@platforms//os:linux"),
        "_macos_constraint": attr.label(default = "@platforms//os:macos"),
        "_windows_constraint": attr.label(default = "@platforms//os:windows"),
    },
    doc = "Install targets",
    toolchains = ["@rules_python//python:toolchain_type"],
)

def mongo_install(
        name,
        srcs,
        deps = [],
        root_files = {},
        include_files = {},
        target_compatible_with = [],
        testonly = False,
        pretty_printer_tests = {},
        archive_license_files = ["//:archive_license_files"],
        package_extract_name = "dist-test",
        publish_debug_in_stripped = False,
        try_zstd = False,
        **kwargs):
    """Perform install actions

    Args:
        name: standard target name
        srcs: list of targets that should be installed
        deps: other install rule targets that should be installed
        **kwargs: other args to pass to underlying rules
        target_compatible_with: forward target_compatible_with args to the rules

    """

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
        # TODO(SERVER-102851): This is commented out because CI AUBSAN builds currently
        # try to build stripped/debug but don't separate them, that should be fixed
        #if install_type:
        #    seperate_debug_incompat = ["@platforms//:incompatible"]

        if len(pretty_printer_tests) > 0 and install_type != "-debug":
            for test_script, test_binary in pretty_printer_tests.items():
                pretty_printer_name = install_target + "-" + test_script.split(":")[-1].split(".")[0]
                mongo_pretty_printer_test(
                    name = "real_" + pretty_printer_name,
                    test_script = test_script,
                    test_binary = test_binary,
                    testonly = True,
                )

                # This is a hacky way to not produce the pretty printer test files on windows and
                # mac - you can't use target_compatible_with because it will skip downstream rules,
                # and in the downstream rules you can't use select or you have nested selects
                empty_rule(
                    name = "fake_" + pretty_printer_name,
                )

                native.alias(
                    name = pretty_printer_name,
                    actual = select({
                        "@platforms//os:linux": "real_" + pretty_printer_name,
                        "//conditions:default": "fake_" + pretty_printer_name,
                    }),
                )
                modified_srcs = modified_srcs + [pretty_printer_name]
            testonly = True

        mongo_install_rule(
            name = install_target,
            srcs = modified_srcs,
            root_files = root_files,
            include_files = include_files,
            debug = debug,
            create_dwp = select({
                "//bazel/config:dwp_supported": True,
                "//bazel/config:create_dwp_disabled": False,
            }, no_match_error = DWP_ERROR_MESSAGE),
            deps = select({
                "//bazel/config:build_enterprise_enabled": dep_targets,
                "//conditions:default": community_dep_targets,
            }),
            target_compatible_with = target_compatible_with + select({
                "//bazel/config:separate_debug_enabled": [],
                "//conditions:default": seperate_debug_incompat,
            }),
            publish_debug_in_stripped = publish_debug_in_stripped,
            testonly = testonly,
            **kwargs
        )

        # This is so the README files dont end up looking like executables
        pkg_files(
            name = install_target + "_licenses",
            srcs = archive_license_files,
            attributes = pkg_attributes(mode = "644"),
        )

        pkg_files(
            name = install_target + "_files",
            srcs = [install_target],
            attributes = pkg_attributes(mode = "755"),
            strip_prefix = install_target,
            testonly = testonly,
        )

        pkg_zip(
            name = "archive-" + name + install_type + "_zip",
            srcs = [install_target + "_files", install_target + "_licenses"],
            package_dir = package_extract_name,
            package_file_name = name + install_type + ".zip",
            exec_properties = {
                "no-cache": "1",
                "no-sandbox": "1",
                "no-remote": "1",
                "local": "1",
            },
            testonly = testonly,
            target_compatible_with = select({
                "@platforms//os:windows": [],
                "//conditions:default": ["@platforms//:incompatible"],
            }),
            **kwargs
        )

        compressor = select({
            "@pigz//:pigz_tool_available": "@pigz//:bin",
            "//conditions:default": None,
        })

        # package up the the install into an archive.
        pkg_tar(
            name = "archive-" + name + install_type + "_tar",
            srcs = [install_target + "_files", install_target + "_licenses"],
            compressor = compressor,
            package_dir = package_extract_name,
            package_file_name = name + install_type + ".tgz",
            extension = "tgz",
            exec_properties = {
                "no-cache": "1",
                "no-sandbox": "1",
                "no-remote": "1",
                "local": "1",
            },
            preserve_mtime = True,
            testonly = testonly,
            target_compatible_with = select({
                "@platforms//os:windows": ["@platforms//:incompatible"],
                "//conditions:default": [],
            }),
            **kwargs
        )
        if try_zstd:
            pkg_tar(
                name = "archive-" + name + install_type + "_zst",
                srcs = [install_target + "_files", install_target + "_licenses"],
                compressor = "@zstd//:bin",
                package_dir = package_extract_name,
                package_file_name = name + install_type + ".zst",
                extension = "zst",
                exec_properties = {
                    "no-cache": "1",
                    "no-sandbox": "1",
                    "no-remote": "1",
                    "local": "1",
                },
                preserve_mtime = True,
                testonly = testonly,
                target_compatible_with = select({
                    "@platforms//os:windows": ["@platforms//:incompatible"],
                    "@zstd//:zstd_tool_not_available": ["@platforms//:incompatible"],
                    "//conditions:default": [],
                }),
                **kwargs
            )

            native.filegroup(
                name = "archive-" + name + install_type,
                srcs = select({
                    "@platforms//os:windows": ["archive-" + name + install_type + "_zip"],
                    "@zstd//:zstd_tool_available": ["archive-" + name + install_type + "_tar", "archive-" + name + install_type + "_zst"],
                    "//conditions:default": ["archive-" + name + install_type + "_tar"],
                }),
                testonly = testonly,
            )
        else:
            native.filegroup(
                name = "archive-" + name + install_type,
                srcs = select({
                    "@platforms//os:windows": ["archive-" + name + install_type + "_zip"],
                    "//conditions:default": ["archive-" + name + install_type + "_tar"],
                }),
                testonly = testonly,
            )

def _extensions_with_config_impl(ctx):
    """Implementation for the extensions_with_config rule."""
    return [DefaultInfo(files = depset(ctx.files.srcs))]

extensions_with_config = rule(
    implementation = _extensions_with_config_impl,
    attrs = {
        "srcs": attr.label_list(cfg = extensions_transition, allow_files = True),
    },
)
