load("//bazel:test_exec_properties.bzl", "test_exec_properties")
load("//bazel/resmoke:.resmoke_suites_derived.bzl", "SUITE_SELECTORS")
load("@rules_python//python:defs.bzl", "py_test")

def _collect_python_imports_impl(ctx):
    """Collects Python imports from data dependencies to build PYTHONPATH."""
    python_imports = []

    # Collect PyInfo from data dependencies
    for dep in ctx.attr.data:
        if PyInfo in dep:
            for import_path in dep[PyInfo].imports.to_list():
                if import_path not in python_imports:
                    python_imports.append(import_path)

    # Write imports to a file, one per line
    imports_file = ctx.actions.declare_file(ctx.label.name + ".python_imports")
    ctx.actions.write(
        output = imports_file,
        content = "\n".join(python_imports) + "\n" if python_imports else "",
    )

    return [DefaultInfo(files = depset([imports_file]))]

_collect_python_imports = rule(
    implementation = _collect_python_imports_impl,
    attrs = {
        "data": attr.label_list(
            allow_files = True,
            doc = "Data dependencies to extract Python imports from",
        ),
    },
    doc = "Helper rule to collect Python imports from data dependencies",
)

def _resmoke_config_impl(ctx):
    """Produces a resmoke config YAML for a suite.

    In passthrough mode, copies the base config verbatim.
    Otherwise, generates a config by replacing roots with resolved srcs.
    """
    base_name = ctx.label.name.removesuffix("_config")
    generated_config_file = ctx.actions.declare_file(base_name + ".yml")
    base_config_file = ctx.files.base_config[0]

    if ctx.attr.passthrough:
        # Copy the original config as-is.
        ctx.actions.symlink(
            output = generated_config_file,
            target_file = base_config_file,
        )
    else:
        # Generate a config with resolved roots from srcs.
        test_list_file = ctx.actions.declare_file(base_name + ".txt")

        python = ctx.toolchains["@rules_python//python:toolchain_type"].py3_runtime
        python_path = []
        for path in ctx.attr.generator[PyInfo].imports.to_list():
            if path not in python_path:
                python_path.append(ctx.expand_make_variables("python_library_imports", "$(BINDIR)/external/" + path, ctx.var))
        generator_deps = [ctx.attr.generator[PyInfo].transitive_sources]

        test_list = [test.short_path for test in ctx.files.srcs]
        ctx.actions.write(test_list_file, "\n".join(test_list))

        deps = depset([test_list_file, base_config_file] + ctx.files.srcs, transitive = [python.files] + generator_deps)

        args = [
            "bazel/resmoke/resmoke_config_generator.py",
            "--output",
            generated_config_file.path,
            "--test-list",
            test_list_file.path,
            "--base-config",
            base_config_file.path,
        ]

        ctx.actions.run(
            executable = python.interpreter.path,
            inputs = deps,
            outputs = [generated_config_file],
            arguments = args,
            env = {"PYTHONPATH": ctx.configuration.host_path_separator.join(python_path)},
        )

    return [DefaultInfo(files = depset([generated_config_file]))]

resmoke_config = rule(
    _resmoke_config_impl,
    attrs = {
        "generator": attr.label(
            doc = "The config generator to use.",
            default = "//bazel/resmoke:resmoke_config_generator",
        ),
        "srcs": attr.label_list(allow_files = True, doc = "Tests to write as the 'roots' of the selector"),
        "passthrough": attr.bool(default = False, doc = "If true, copy the base config verbatim instead of generating."),
        "base_config": attr.label(
            allow_files = True,
            doc = "The base resmoke YAML config for the suite",
        ),
    },
    doc = "Produces a resmoke config YAML. In passthrough mode, copies the base config verbatim. Otherwise, replaces roots with resolved srcs.",
    toolchains = ["@rules_python//python:toolchain_type"],
)

def _resolve_suite_srcs(config):
    """Resolve srcs for a suite from the auto-generated selector data.

    Extracts the config label's target path and looks it up in SUITE_SELECTORS.
    Returns the list of srcs labels (possibly empty for no-roots test kinds),
    or None if the config was not found in SUITE_SELECTORS at all.
    """
    config_str = str(config)

    # Try direct lookup first (config is already a full label like
    # "//buildscripts/resmokeconfig:suites/auth.yml")
    if config_str in SUITE_SELECTORS:
        return SUITE_SELECTORS[config_str]

    # Handle Label objects: convert to string "//pkg:target"
    if hasattr(config, "package") and hasattr(config, "name"):
        label_str = "//%s:%s" % (config.package, config.name)
        if label_str in SUITE_SELECTORS:
            return SUITE_SELECTORS[label_str]

    return None

def resmoke_suite_test(
        name,
        config,
        data = [],
        deps = [],
        resmoke_args = [],
        size = "small",
        srcs = [],
        tags = [],
        timeout = "eternal",
        exec_properties = {},
        multiversion_deps = [],
        **kwargs):
    """Creates a Bazel test target for a resmoke suite.

    The suite's test files (srcs) are automatically derived from the YAML
    config's selector.roots field via pre-build generation. When srcs are
    auto-derived, the original YAML config is passed directly to resmoke.
    When explicit srcs are provided, a generated config with resolved roots
    is created instead.

    Args:
        name: Target name.
        config: Label of the resmoke suite YAML config file.
        data: Additional data dependencies (JS libraries, certs, etc.).
        deps: Binary dependencies (mongod, mongos, etc.).
        resmoke_args: Additional command-line arguments for resmoke.
        size: Bazel test size.
        srcs: Override for test source files. If empty, auto-derived from config.
        tags: Bazel tags.
        timeout: Bazel test timeout.
        exec_properties: Execution properties for remote execution.
        multiversion_deps: List of multiversion_setup targets whose output
            directories are passed to resmoke as --multiversionDir entries.
        **kwargs: Additional arguments passed to py_test (e.g., shard_count).
    """

    # Auto-derive srcs from the suite YAML if not explicitly provided.
    passthrough = not srcs
    if not srcs:
        resolved = _resolve_suite_srcs(config)
        if resolved == None:
            fail("resmoke_suite_test '%s': no srcs provided and config '%s' not found in SUITE_SELECTORS. " +
                 "Either provide explicit srcs or ensure the suite YAML has selector.roots." % (name, config))
        srcs = resolved

    generated_config = name + "_config"
    resmoke_config(
        name = generated_config,
        srcs = srcs,
        base_config = config,
        passthrough = passthrough,
        tags = ["resmoke_config"],
    )

    historic_runtimes = name + "_historic_runtimes"
    native.genrule(
        name = historic_runtimes,
        srcs = [],
        outs = [historic_runtimes + ".json"],
        cmd = "$(location //bazel/resmoke:download_historic_runtimes) --suite=//{pkg}:{name} --volatile-status=bazel-out/volatile-status.txt --output=$@".format(pkg = native.package_name(), name = name),
        tools = ["//bazel/resmoke:download_historic_runtimes"],
        stamp = True,
        tags = ["no-remote", "external", "no-cache"],
    )

    # Collect Python imports from data dependencies
    python_imports_target = name + "_python_imports"
    _collect_python_imports(
        name = python_imports_target,
        data = data,
        tags = ["manual"],
    )

    resmoke_shim = Label("//bazel/resmoke:resmoke_shim.py")
    resmoke = Label("//buildscripts:resmoke")
    extra_args = select({
        "//bazel/resmoke:in_evergreen_enabled": [
            "--log=evg",
            "--cedarReportFile=cedar_report.json",
            "--skipSymbolization",  # Symbolization is not yet functional, SERVER-103538
            "--continueOnFailure",
        ],
        "//conditions:default": [],
    }) + select({
        "//bazel/resmoke:installed_dist_test_enabled": [
            "--installDir=dist-test/bin",
            "--mongoVersionFile=$(location //:.resmoke_mongo_version.yml)",
        ],
        "//conditions:default": [
            "--mongoVersionFile=$(location //bazel/resmoke:resmoke_mongo_version)",
        ],
    })

    deps_path = ":".join(["$(location %s)" % dep for dep in deps])

    default_data = [
        generated_config,
        python_imports_target,
        "//bazel/resmoke:on_feature_flags",
        "//bazel/resmoke:off_feature_flags",
        "//bazel/resmoke:unreleased_ifr_flags",
        "//bazel/resmoke:all_ifr_flags",
        "//bazel/resmoke:volatile_status",
        "//bazel/resmoke:resource_monitor",
        ":%s" % historic_runtimes,
        "//buildscripts/resmokeconfig:common_jstest_data",
        "//buildscripts/resmokeconfig:required_jstest_data",
        "//buildscripts/resmokeconfig:fully_disabled_feature_flags.yml",
        "//buildscripts/resmokeconfig:resmoke_modules.yml",
        "//buildscripts/resmokeconfig/evg_task_doc:all_files",
        "//buildscripts/resmokeconfig/loggers:all_files",
        "//src/mongo/util/version:releases.yml",
        "//:generated_resmoke_config",
        "//:jsconfig.json",

        # The below dependencies are used by many suites. To ease authoring new suites, they
        # are included in all suites for now.
        # TODO(SERVER-122756), prune this, ideally removing it entirely.
        "//jstests/libs:authTestsKey",
        "//jstests/libs:key1",
        "//jstests/libs:key2",
        "//src/third_party/schemastore.org:schemas",
        "//x509:generate_main_certificates",
    ]
    multiversion_config = ["//bazel/resmoke:multiversion_config"] if multiversion_deps else []

    # Each multiversion_setup target "last-lts" also produces a "last-lts_exclude_tags"
    multiversion_exclude_tags = [
        (dep.rsplit(":", 1)[0] + ":" + dep.rsplit(":", 1)[1] + "_exclude_tags") if ":" in dep else dep + "_exclude_tags"
        for dep in multiversion_deps
    ]

    merged_data = data + [d for d in srcs if d not in data] + [d for d in default_data if d not in data and d not in srcs] + multiversion_deps + multiversion_config + multiversion_exclude_tags

    py_test(
        name = name,
        srcs = [resmoke_shim],
        data = merged_data + select({
            "//bazel/resmoke:installed_dist_test_enabled": ["//:installed-dist-test", "//:.resmoke_mongo_version.yml"],
            "//conditions:default": ["//bazel/resmoke:resmoke_mongo_version"],
        }),
        deps = [
            resmoke,
            "//buildscripts:bazel_local_resources",
        ] + select({
            "//bazel/resmoke:installed_dist_test_enabled": [],
            "//bazel/resmoke:skip_deps_for_cquery_enabled": [],
            "//conditions:default": deps,
        }),
        main = resmoke_shim,
        args = [
            "run",
            "--suites=$(location %s)" % native.package_relative_label(generated_config),
            "--releasesFile=$(location //src/mongo/util/version:releases.yml)",
            "--archiveMode=directory",
            "--archiveLimitMb=500",
            "--testTimeout=$(RESMOKE_TEST_TIMEOUT)",
            "--historicTestRuntimes=$(location :%s)" % historic_runtimes,
        ] + [
            "--multiversionDir=$(location %s)" % native.package_relative_label(dep)
            for dep in multiversion_deps
        ] + [
            "--tagFile=$(location %s)" % native.package_relative_label(tag)
            for tag in multiversion_exclude_tags
        ] + extra_args + resmoke_args,
        tags = tags + ["no-cache", "resources:port_block:1", "resmoke_suite_test"],
        timeout = timeout,
        size = size,
        env = {
            "LOCAL_RESOURCES": "$(LOCAL_RESOURCES)",
            "GIT_PYTHON_REFRESH": "quiet",  # Ignore "Bad git executable" error when importing git python. Git commands will still error if run.
            "PYTHON_IMPORTS_FILE": "$(location %s)" % native.package_relative_label(python_imports_target),
        } | ({
            "MULTIVERSION_CONFIG_FILE": "$(location //bazel/resmoke:multiversion_config)",
            "MULTIVERSION_VERSIONS": ",".join([
                dep.rsplit(":", 1)[1] if ":" in dep else dep
                for dep in multiversion_deps
            ]),
        } if multiversion_deps else {}) | select({
            "//bazel/resmoke:installed_dist_test_enabled": {},
            "//bazel/resmoke:skip_deps_for_cquery_enabled": {},
            "//conditions:default": {"DEPS_PATH": deps_path},
        }),
        exec_properties = exec_properties | test_exec_properties(tags),
        toolchains = [
            "//bazel/resmoke:test_timeout",
        ],
        **kwargs
    )
