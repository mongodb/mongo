load("//bazel/install_rules:install_rules.bzl", "MongoInstallInfo")

def get_from_volatile_status(ctx, key):
    return "`grep '^" + key + " ' " + ctx.version_file.short_path + " | cut -d' ' -f2`"

def _resmoke_suite_test_impl(ctx):
    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    python_path = []
    for path in ctx.attr.resmoke[PyInfo].imports.to_list():
        if path not in python_path:
            python_path.append("$TEST_SRCDIR/" + path)
    python_path = ctx.configuration.host_path_separator.join(python_path)

    # Put install binaries on the path for resmoke. The MongoInstallInfo has each binary in its own
    # directory.
    binary_path = []
    for data in ctx.attr.data:
        if MongoInstallInfo in data:
            for file in data.files.to_list():
                binary_path.append("$TEST_SRCDIR/_main/" + file.short_path.replace("bin/" + file.basename, "bin/"))
    binary_path = ctx.configuration.host_path_separator.join(binary_path)

    suite = ctx.attr.config.files.to_list()[0].path

    resmoke_args = [
        "run",
        "--dbpathPrefix=$TEST_UNDECLARED_OUTPUTS_DIR/data",
        "--taskWorkDir=$TEST_UNDECLARED_OUTPUTS_DIR",
        "--multiversionDir=multiversion_binaries",
        "--noValidateSelectorPaths",  # Skip validating selector paths. Excluded files in a suite config should not be required dependencies.
        "--continueOnFailure",
        "--suite",
        suite,
    ] + ctx.attr.resmoke_args

    if ctx.attr.evergreen_format:
        resmoke_args.append("--installDir=dist-test/bin")
        resmoke_args.append("--log=evg")
        resmoke_args.append("--reportFile=$TEST_UNDECLARED_OUTPUTS_DIR/report.json")
        resmoke_args.append("--cedarReportFile=cedar_report.json")

        # Symbolization is not yet functional, SERVER-103538
        resmoke_args.append("--skipSymbolization")

        resmoke_args.append("--buildId=" + get_from_volatile_status(ctx, "build_id"))
        resmoke_args.append("--distroId=" + get_from_volatile_status(ctx, "distro_id"))
        resmoke_args.append("--executionNumber=" + get_from_volatile_status(ctx, "execution"))
        resmoke_args.append("--projectName=" + get_from_volatile_status(ctx, "project"))
        resmoke_args.append("--gitRevision=" + get_from_volatile_status(ctx, "revision"))
        resmoke_args.append("--revisionOrderId=" + get_from_volatile_status(ctx, "revision_order_id"))
        resmoke_args.append("--taskId=" + get_from_volatile_status(ctx, "task_id"))
        resmoke_args.append("--taskName=" + get_from_volatile_status(ctx, "task_name"))
        resmoke_args.append("--variantName=" + get_from_volatile_status(ctx, "build_variant"))
        resmoke_args.append("--versionId=" + get_from_volatile_status(ctx, "version_id"))
        resmoke_args.append("--requester=" + get_from_volatile_status(ctx, "requester"))

    # This script is the action to run resmoke. It looks like this (abbreviated):
    # PATH=$TEST_SRCDIR/_main/install-mongo/bin/:$PATH PYTHONPATH=$TEST_SRCDIR/poetry/packaging python3 resmoke.py run ... $@
    # The $@ allows passing extra arguments to resmoke using --test_arg in the bazel invocation.
    script = "ln -sf $TEST_SRCDIR/_main/bazel/resmoke/.resmoke_mongo_version.yml $TEST_SRCDIR/_main/\n"  # workaround for resmoke assuming this location.
    script = script + "PATH=" + binary_path + ":$PATH"
    script = script + " " + "PYTHONPATH=" + python_path + " "
    script = script + python.interpreter.path + " buildscripts/resmoke.py " + " ".join(resmoke_args) + " $@"
    ctx.actions.write(
        output = ctx.outputs.executable,
        content = script,
    )

    resmoke_deps = [ctx.attr.resmoke[PyInfo].transitive_sources]
    deps = depset(transitive = [python.files] + resmoke_deps)
    runfiles = ctx.runfiles(files = deps.to_list() + ctx.files.data + ctx.files.deps + ctx.files.srcs + [ctx.version_file])
    return [DefaultInfo(runfiles = runfiles)]

_resmoke_suite_test = rule(
    implementation = _resmoke_suite_test_impl,
    attrs = {
        "config": attr.label(allow_files = True),
        "data": attr.label_list(allow_files = True),
        "deps": attr.label_list(allow_files = True),
        "needs_mongo": attr.bool(default = False),
        "needs_mongod": attr.bool(default = False),
        "resmoke": attr.label(default = "//buildscripts:resmoke"),
        "resmoke_args": attr.string_list(),
        "srcs": attr.label_list(allow_files = True),
        "evergreen_format": attr.bool(default = False),
    },
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
    test = True,
)

def resmoke_suite_test(
        name,
        config,
        data = [],
        deps = [],
        resmoke_args = [],
        srcs = [],
        tags = [],
        timeout = "eternal",
        needs_mongo = False,
        needs_mongod = False,
        needs_mongos = False,
        **kwargs):
    install_deps = []
    if needs_mongo:
        install_deps.append("//:install-mongo")
    if needs_mongod:
        install_deps.append("//:install-mongod")
    if needs_mongos:
        install_deps.append("//:install-mongos")

    _resmoke_suite_test(
        name = name,
        config = config,
        data = data + [
            config,
            "//bazel/resmoke:resmoke_mongo_version",
            "//bazel/resmoke:off_feature_flags",
            "//buildscripts/resmokeconfig:all_files",  # This needs to be reduced, SERVER-103610
            "//src/mongo/util/version:releases.yml",
        ] + select({
            "//bazel/resmoke:in_evergreen_enabled": ["//:installed-dist-test"],
            "//conditions:default": install_deps,
        }),
        srcs = srcs,
        deps = deps,
        resmoke_args = resmoke_args,
        timeout = timeout,
        tags = tags + ["local", "no-cache"],
        evergreen_format = select({
            "//bazel/resmoke:in_evergreen_enabled": True,
            "//conditions:default": False,
        }),
        **kwargs
    )
