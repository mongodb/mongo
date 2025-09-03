def resmoke_config_impl(ctx):
    base_name = ctx.label.name.removesuffix("_config")
    test_list_file = ctx.actions.declare_file(base_name + ".txt")
    generated_config_file = ctx.actions.declare_file(base_name + ".yml")
    base_config_file = ctx.files.base_config[0]

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    python_path = []
    for path in ctx.attr.generator[PyInfo].imports.to_list():
        if path not in python_path:
            python_path.append(ctx.expand_make_variables("python_library_imports", "$(BINDIR)/external/" + path, ctx.var))
    generator_deps = [ctx.attr.generator[PyInfo].transitive_sources]

    test_list = [test.short_path for test in ctx.files.srcs]
    for exclude in [test.short_path for test in ctx.files.exclude_files]:
        if exclude in test_list:
            test_list.remove(exclude)
    ctx.actions.write(test_list_file, "\n".join(test_list))

    deps = depset([test_list_file, base_config_file] + ctx.files.srcs, transitive = [python.files] + generator_deps)

    ctx.actions.run(
        executable = python.interpreter.path,
        inputs = deps,
        outputs = [generated_config_file],
        arguments = [
            "bazel/resmoke/resmoke_config_generator.py",
            "--output",
            generated_config_file.path,
            "--test-list",
            test_list_file.path,
            "--base-config",
            base_config_file.path,
            "--exclude-with-any-tags",
            ",".join(ctx.attr.exclude_with_any_tags),
            "--include-with-any-tags",
            ",".join(ctx.attr.include_with_any_tags),
        ],
        env = {"PYTHONPATH": ctx.configuration.host_path_separator.join(python_path)},
    )

    return [DefaultInfo(files = depset([generated_config_file]))]

resmoke_config = rule(
    resmoke_config_impl,
    attrs = {
        "generator": attr.label(
            doc = "The config generator to use.",
            default = "//bazel/resmoke:resmoke_config_generator",
        ),
        "srcs": attr.label_list(allow_files = True, doc = "Tests to write as the 'roots' of the selector"),
        "exclude_files": attr.label_list(allow_files = True),
        "exclude_with_any_tags": attr.string_list(),
        "include_with_any_tags": attr.string_list(),
        "base_config": attr.label(
            allow_files = True,
            doc = "The base resmoke YAML config for the suite",
        ),
    },
    doc = "Generates a resmoke config YAML",
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
)

def resmoke_suite_test(
        name,
        config,
        data = [],
        deps = [],
        exclude_files = [],
        exclude_with_any_tags = [],
        include_with_any_tags = [],
        resmoke_args = [],
        size = "small",
        srcs = [],
        tags = [],
        timeout = "eternal",
        **kwargs):
    generated_config = name + "_config"
    resmoke_config(
        name = generated_config,
        srcs = srcs,
        exclude_files = exclude_files,
        base_config = config,
        exclude_with_any_tags = exclude_with_any_tags,
        include_with_any_tags = include_with_any_tags,
        tags = ["resmoke_config"],
    )

    resmoke_shim = Label("//bazel/resmoke:resmoke_shim.py")
    resmoke = Label("//buildscripts:resmoke")
    extra_args = select({
        "//bazel/resmoke:in_evergreen_enabled": [
            "--log=evg",
            "--cedarReportFile=cedar_report.json",
            "--skipSymbolization",  # Symbolization is not yet functional, SERVER-103538
            "--installDir=dist-test/bin",
            "--mongoVersionFile=$(location //:.resmoke_mongo_version.yml)",
        ],
        "//conditions:default": [
            "--installDir=install-dist-test/bin",
            "--mongoVersionFile=$(location //bazel/resmoke:resmoke_mongo_version)",
        ],
    })
    native.py_test(
        name = name,
        # To a user of resmoke_suite_test, the `srcs` is the list of tests to select. However, to the py_test rule,
        # the `srcs` are expected to be Python files only.
        srcs = [resmoke_shim],
        data = data + srcs + [
            config,
            generated_config,
            "//bazel/resmoke:on_feature_flags",
            "//bazel/resmoke:off_feature_flags",
            "//bazel/resmoke:unreleased_ifr_flags",
            "//bazel/resmoke:volatile_status",
            "//bazel/resmoke:test_runtimes",
            "//buildscripts/resmokeconfig:fully_disabled_feature_flags.yml",
            "//buildscripts/resmokeconfig:resmoke_modules.yml",
            "//buildscripts/resmokeconfig/evg_task_doc:all_files",
            "//buildscripts/resmokeconfig/loggers:all_files",
            "//src/mongo/util/version:releases.yml",
            "//:generated_resmoke_config",
        ] + select({
            "//bazel/resmoke:in_evergreen_enabled": ["//:installed-dist-test", "//:.resmoke_mongo_version.yml"],
            "//conditions:default": ["//:install-dist-test", "//bazel/resmoke:resmoke_mongo_version"],
        }),
        deps = deps + [
            resmoke,
            "//buildscripts:bazel_local_resources",
        ],
        main = resmoke_shim,
        args = [
            "run",
            "--suites=$(location %s)" % native.package_relative_label(generated_config),
            "--multiversionDir=multiversion_binaries",
            "--continueOnFailure",
            "--releasesFile=$(location //src/mongo/util/version:releases.yml)",
        ] + extra_args + resmoke_args,
        tags = tags + ["no-cache", "local", "resources:port_block:1"],
        timeout = timeout,
        size = size,
        env = {
            "LOCAL_RESOURCES": "$(LOCAL_RESOURCES)",
            "GIT_PYTHON_REFRESH": "quiet",  # Ignore "Bad git executable" error when importing git python. Git commands will still error if run.
        },
        **kwargs
    )
