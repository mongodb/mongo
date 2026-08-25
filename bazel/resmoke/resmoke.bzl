"""Resmoke suite test infrastructure for Bazel."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("//bazel/config:py_action_env.bzl", "py_exec_import_paths")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")
load("@rules_python//python:defs.bzl", "py_binary", "py_test")
load("@rules_python//python:py_info.bzl", "PyInfo")
load("//bazel:test_exec_properties.bzl", "test_exec_properties")
load("//bazel/resmoke:.resmoke_suites_derived.bzl", "SUITE_SELECTORS")

def _tss_settings_file_impl(ctx):
    """Materializes the test selection build settings so a genrule can read them.

    genrules cannot read build settings, so the values are written to a file the test-list genrule
    takes as an input: first line whether selection is enabled, second line the strategies.
    """
    settings_file = ctx.actions.declare_file(ctx.label.name + ".txt")
    enabled = ctx.attr._enable_flag[BuildSettingInfo].value
    strategies = ctx.attr._strategies_flag[BuildSettingInfo].value
    ctx.actions.write(settings_file, "{}\n{}\n".format("true" if enabled else "false", strategies))
    return [DefaultInfo(files = depset([settings_file]))]

tss_settings_file = rule(
    implementation = _tss_settings_file_impl,
    attrs = {
        "_enable_flag": attr.label(
            default = "//bazel/resmoke:enable_test_selection",
            providers = [BuildSettingInfo],
        ),
        "_strategies_flag": attr.label(
            default = "//bazel/resmoke:test_selection_strategies",
            providers = [BuildSettingInfo],
        ),
    },
    doc = "Writes the test selection build settings to a file for the test list genrule.",
)

def _config_fuzz_seed_file_impl(ctx):
    seed_file = ctx.actions.declare_file(ctx.label.name + ".txt")
    fixed_seed = ctx.attr._seed_flag[BuildSettingInfo].value

    if fixed_seed:
        ctx.actions.write(seed_file, fixed_seed + "\n")
    else:
        ctx.actions.run_shell(
            inputs = [ctx.version_file],
            outputs = [seed_file],
            command = """python3 -c '
import hashlib, sys
with open(sys.argv[1]) as f:
    content = f.read()
fields = dict(line.split(None, 1) for line in content.splitlines() if " " in line)
raw = fields.get("BUILD_TIMESTAMP", "") + "|" + sys.argv[2]
seed = int(hashlib.sha256(raw.strip().encode()).hexdigest(), 16) % sys.maxsize
print(seed)
' "$1" "$2" > "$3"
""",
            arguments = [ctx.version_file.path, ctx.attr.suite_name, seed_file.path],
            execution_requirements = {
                "no-cache": "1",
                "no-remote": "1",
            },
        )

    return [DefaultInfo(files = depset([seed_file]))]

_config_fuzz_seed_file = rule(
    implementation = _config_fuzz_seed_file_impl,
    attrs = {
        "_seed_flag": attr.label(
            default = "//bazel/resmoke:config_fuzz_seed",
            providers = [BuildSettingInfo],
        ),
        "suite_name": attr.string(
            mandatory = True,
            doc = "Name of the resmoke suite, combined with BUILD_TIMESTAMP to produce a per-suite seed.",
        ),
    },
    doc = "Generates a seed file for config fuzzer suites. When --//bazel/resmoke:config_fuzz_seed is empty (default), derives the seed from BUILD_TIMESTAMP and the suite name in volatile-status so all shards share the same seed. When set, writes that value verbatim.",
)

def _historic_runtime_impl(ctx):
    output = ctx.outputs.out
    args = ctx.actions.args()
    args.add("--suite", ctx.attr.suite)
    args.add("--volatile-status", ctx.version_file.path)
    args.add("--output", output.path)

    ctx.actions.run(
        executable = ctx.executable._downloader,
        arguments = [args],
        inputs = [ctx.version_file],
        tools = [ctx.attr._downloader[DefaultInfo].files_to_run],
        outputs = [output],
        mnemonic = "HistoricRuntime",
        execution_requirements = {
            "no-cache": "1",
            "no-remote": "1",
            "requires-network": "1",
        },
        use_default_shell_env = True,
    )

    return [DefaultInfo(files = depset([output]))]

_historic_runtime = rule(
    implementation = _historic_runtime_impl,
    attrs = {
        "out": attr.output(mandatory = True),
        "suite": attr.string(mandatory = True),
        "_downloader": attr.label(
            default = Label("//bazel/resmoke:download_historic_runtimes"),
            executable = True,
            cfg = "exec",
        ),
    },
)

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

def _resmoke_deps_path_impl(ctx):
    paths = []
    for dep in ctx.attr.deps:
        files_to_run = dep[DefaultInfo].files_to_run
        if files_to_run.executable:
            paths.append(files_to_run.executable.path)
            continue

        files = dep[DefaultInfo].files.to_list()
        if len(files) != 1:
            fail(
                "%s must produce exactly one file for resmoke DEPS_PATH, got %d" %
                (dep.label, len(files)),
            )
        paths.append(files[0].path)

    output = ctx.actions.declare_file(ctx.label.name + ".txt")
    ctx.actions.write(
        output = output,
        content = "\n".join(paths) + "\n" if paths else "",
    )
    return [DefaultInfo(files = depset([output]))]

_resmoke_deps_path = rule(
    implementation = _resmoke_deps_path_impl,
    attrs = {
        "deps": attr.label_list(
            allow_files = True,
            doc = "Binary dependencies whose configured executable paths should be passed to resmoke.",
        ),
    },
    doc = "Writes configured resmoke binary dependency paths for host-side cross-hermetic_container execution.",
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
        # Generate a config with resolved roots from srcs, or from explicit root
        # strings when those are supplied.
        test_list_file = ctx.actions.declare_file(base_name + ".txt")

        python = ctx.toolchains["@rules_python//python:toolchain_type"].py3_runtime

        # generator is `cfg = "exec"`: the config-generation action runs the
        # exec-platform interpreter, so its deps' wheels must be exec-config
        # too. See py_exec_import_paths for the path derivation.
        python_path = py_exec_import_paths(ctx, [ctx.attr.generator])
        generator_deps = [ctx.attr.generator[PyInfo].transitive_sources]

        if ctx.attr.roots:
            # Roots are supplied as explicit runfiles-relative path/glob strings
            # (see the config_roots arg of resmoke_suite_test). Write them verbatim
            # and take no dependency on srcs, so the config can be built without
            # building the (potentially very large) test corpus.
            # resmoke expands any glob patterns in roots at run time.
            test_list = ctx.attr.roots
        elif ctx.attr.test_root_granularity == "directory":
            # The test kind treats each directory as a single test case (e.g.
            # query_tester). Track the individual files as inputs for hermeticity
            # but write the deduped parent directories as roots. A directory tree
            # artifact is already the test-case root; a plain file contributes its
            # parent directory.
            test_list = sorted({
                (test.short_path if test.is_directory else test.short_path.rsplit("/", 1)[0]): None
                for test in ctx.files.srcs
            }.keys())
        else:
            # Directory artifacts (e.g. from jstestfuzz_generate) can't be enumerated
            # so record them as a glob.
            test_list = [
                (test.short_path + "/*.js") if test.is_directory else test.short_path
                for test in ctx.files.srcs
            ]
        ctx.actions.write(test_list_file, "\n".join(test_list))

        deps = depset([test_list_file, base_config_file], transitive = [python.files] + generator_deps)

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
            cfg = "exec",
        ),
        "srcs": attr.label_list(allow_files = True, doc = "Tests to write as the 'roots' of the selector"),
        "roots": attr.string_list(
            doc = "Explicit path/glob strings, relative to resmoke's working directory, " +
                  "to write as the selector 'roots', " +
                  "used instead of deriving them from srcs. Takes precedence over srcs.",
        ),
        "test_root_granularity": attr.string(
            default = "file",
            values = ["file", "directory"],
            doc = "Granularity of a test case: 'file' (default, one root per src file) or " +
                  "'directory' (one root per src's parent directory, for test kinds like " +
                  "query_tester that treat each directory as a test case).",
        ),
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

# Python helper programs included in every suite's data (see default_data below).
# jstests spawn these as subprocesses through RESMOKE_PYTHON, so their PyInfo
# imports have to reach PYTHONPATH via _collect_python_imports — being in
# default_data alone only puts the sources in runfiles, not their dependencies'
# import paths. Kept as a separate list so both uses stay in sync.
_DEFAULT_PYTHON_DATA = [
    "//jstests/ocsp/lib:ocsp_mock",
    "//jstests/sharding/libs:proxy_protocol_server",
    "//jstests/ssl:tls_enumerator",
    "//src/mongo/db/modules/enterprise/jstests/external_auth/lib:ldapmockserver",
]

# Bazel fails analysis for a native shard_count above this ("Having more than 50 shards is
# indicative of poor test organization"). Suites needing more go through the _forced_shards_transition.
_MAX_NATIVE_SHARD_COUNT = 50

def _resolve_config_root(name, root):
    """Resolves a '//path' config root to a path relative to resmoke's cwd.

    resmoke_shim.py runs resmoke from a short working directory and symlinks the
    main repo's runfiles entries into it at the top level, so a root is just the
    workspace-relative path with the leading '//' stripped. Paths produced by a
    rule are written the same way: runfiles drop the bazel-out prefix.
    """
    if not root.startswith("//"):
        fail(("resmoke_suite_test '%s': config_roots entry '%s' must be a workspace-relative " +
              "path written as '//path/to/tests/*/'.") % (name, root))
    return root[len("//"):]

def _dep_target_name(dep):
    """Extract the target name from a label string, e.g. '//pkg:name' → 'name'."""
    if ":" in dep:
        return dep.rsplit(":", 1)[1]
    return dep.rsplit("/", 1)[-1]

def resmoke_suite_test(
        name,
        config,
        data = [],
        deps = [],
        resmoke_args = [],
        srcs = [],
        config_roots = [],
        tags = [],
        target_compatible_with = [],
        test_root_granularity = "file",
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
        srcs: Override for test source files. If empty, auto-derived from config.
        config_roots: Explicit selector root path/glob strings for the generated
            config, interpreted relative to resmoke's working directory. Each entry
            must be a workspace-relative path written as '//path/to/tests/*/',
            including a path produced by a rule (runfiles drop the bazel-out
            prefix). When set, the *_config target writes these roots verbatim and
            takes no dependency on srcs, so it can be built without building the
            (potentially very large) test corpus; resmoke expands glob patterns at
            run time. srcs is still used for the test's runtime data. Use for
            suites whose corpus is a directory produced by a rule (e.g.
            query_correctness), where the individual test files are not known at
            analysis time.
            Note that a root naming a directory must be written as a glob
            ('.../*/'); resmoke silently discards non-glob roots that are not
            existing files.
        test_root_granularity: Granularity of a test case: "file" (default, one
            root per src file) or "directory" (one root per src's parent
            directory). Use "directory" for test kinds that treat each directory
            as a single test case (e.g. query_tester_server_test).
        tags: Bazel tags.
        target_compatible_with: Compatibility constraints forwarded to py_test.
            Suites whose multiversion_deps consist solely of last-continuous
            automatically gain an additional incompatibility with
            @platforms//:incompatible when
            //bazel/resmoke/multiversion:last_continuous_redundant is True.
        timeout: Bazel test timeout.
        exec_properties: Execution properties for remote execution.
        multiversion_deps: List of multiversion_setup targets whose output
            directories are passed to resmoke as --multiversionDir entries.
        **kwargs: Additional arguments passed to py_test (e.g., shard_count).
    """

    # Auto-derive srcs from the suite YAML if neither srcs nor config_roots is
    # explicitly provided.
    passthrough = not srcs and not config_roots
    if passthrough:
        resolved = _resolve_suite_srcs(config)
        if resolved == None:
            fail(("resmoke_suite_test '%s': no srcs provided and config '%s' not found in SUITE_SELECTORS. " +
                  "Either provide explicit srcs or ensure the suite YAML has selector.roots.") % (name, config))
        srcs = resolved

    generated_config = name + "_config"
    resmoke_config(
        name = generated_config,
        # When config_roots is set, the config is generated from those strings and
        # must not depend on srcs, so building *_config does not fetch the corpus.
        srcs = [] if config_roots else srcs,
        roots = [_resolve_config_root(name, root) for root in config_roots],
        base_config = config,
        passthrough = passthrough,
        test_root_granularity = test_root_granularity,
        tags = ["resmoke_config"],
    )

    historic_runtimes = name + "_historic_runtimes"

    # Runtime data comes from S3, so it cannot run on RBE. It is still build
    # work rather than test execution: route this distinct mnemonic to the
    # local pinned container through Bazel's persistent-container strategy.
    _historic_runtime(
        name = historic_runtimes,
        out = historic_runtimes + ".json",
        suite = "//{pkg}:{name}".format(pkg = native.package_name(), name = name),
    )

    # Collect Python imports from data dependencies
    python_imports_target = name + "_python_imports"
    _collect_python_imports(
        name = python_imports_target,
        data = select({
            "//bazel/resmoke:skip_deps_for_cquery_enabled": _DEFAULT_PYTHON_DATA,
            "//conditions:default": _DEFAULT_PYTHON_DATA +
                                    [d for d in data if d not in _DEFAULT_PYTHON_DATA],
        }),
        tags = ["manual"],
    )

    # Config fuzzer: pre-generate a shared seed so all shards use the same fuzz
    # config, and so the seed is available for the reproduction command.
    is_config_fuzzer = (
        not any([arg.startswith("--configFuzzSeed") for arg in resmoke_args]) and
        any([
            arg.startswith("--fuzzMongodConfigs") or arg.startswith("--fuzzMongosConfigs")
            for arg in resmoke_args
        ])
    )
    seed_target_data = []
    seed_env = {}
    if is_config_fuzzer:
        seed_target = name + "_config_fuzz_seed"
        _config_fuzz_seed_file(
            name = seed_target,
            suite_name = name,
            tags = ["manual"],
        )
        seed_target_data = [":%s" % seed_target]
        seed_env = {"CONFIG_FUZZ_SEED_FILE": "$(location :%s)" % seed_target}

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
        ],
        "//conditions:default": [],
    })

    deps_path = ":".join(["$(location %s)" % dep for dep in deps])
    deps_path_file = name + "_resmoke_deps_path"
    _resmoke_deps_path(
        name = deps_path_file,
        deps = deps,
        tags = [
            "manual",
            "resmoke_deps_path",
        ],
    )

    default_data = _DEFAULT_PYTHON_DATA + [
        generated_config,
        python_imports_target,
        "//bazel/resmoke:resmoke_mongo_version",
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
        "//jstests/aggregation/extras:merge_helpers.js",
        "//jstests/auth:all_javascript_files",
        "//jstests/auth/lib:all_subpackage_javascript_files",
        "//jstests/concurrency/fsm_libs:all_javascript_files",
        "//jstests/concurrency/fsm_libs:shard_fixture.js",
        "//jstests/concurrency/fsm_utils:all_javascript_files",
        "//jstests/concurrency/reshard_collection_util:all_javascript_files",
        "//jstests/core/catalog/libs:all_javascript_files",
        "//jstests/core/libs:raw_operation_utils.js",
        "//jstests/disk/libs:all_javascript_files",
        "//jstests/libs:8k-prime.dhparam",
        "//jstests/libs:authTestsKey",
        "//jstests/libs:key1",
        "//jstests/libs:key2",
        "//jstests/libs:keyForRollover",
        "//jstests/libs:test_pem_files",
        "//jstests/libs/config_files:disable_auth.ini",
        "//jstests/libs/config_files:disable_noauth.ini",
        "//jstests/libs/config_files:enable_auth.json",
        "//jstests/libs/config_files:set_replsetname.json",
        "//jstests/libs/config_files:set_shardingrole_configsvr.json",
        "//jstests/libs/config_files:set_shardingrole_shardsvr.json",
        "//jstests/libs/config_files:timezone_info",
        "//jstests/multiVersion/libs:all_javascript_files",
        "//jstests/noPassthrough/libs:server_parameter_helpers.js",
        "//jstests/noPassthrough/libs/index_builds:index_build.js",
        "//jstests/noPassthrough/memory_tracking:all_javascript_files",
        "//jstests/noPassthrough/rs_endpoint/lib:all_subpackage_javascript_files",
        "//jstests/ocsp/lib:all_javascript_files",
        "//jstests/query_golden/libs:all_javascript_files",
        "//jstests/query_golden/test_inputs:all_javascript_files",
        "//jstests/query_golden/expected_output:expected_output",
        "//jstests/query_golden_sharding:all_javascript_files",
        "//jstests/query_golden_sharding/expected_output:expected_output",
        "//jstests/readonly/lib:all_javascript_files",
        "//jstests/sharding/analyze_shard_key/libs:all_javascript_files",
        "//jstests/sharding/libs:all_javascript_files",
        "//jstests/sharding/libs:last_lts_mongod_commands.js",
        "//jstests/sharding/libs:last_lts_mongos_commands.js",
        "//jstests/ssl/libs:all_javascript_files",
        "//jstests/with_mongot:keyfile_for_testing",
        "//jstests/with_mongot/search_mocked/lib:all_javascript_files",
        "//jstests/with_mongot/search_mocked/ssl/lib:all_javascript_files",
        "//jstests/write_path/timeseries/pbt/geodata:all_javascript_files",
        "//jstests/write_path/timeseries/pbt/lib:all_javascript_files",
        "//src/mongo/db/modules/enterprise/jstests/audit/lib:all_javascript_files",
        "//src/mongo/db/modules/enterprise/jstests/audit/lib:localKey",
        "//src/mongo/db/modules/enterprise/jstests/encryptdb/libs:all_javascript_files",
        "//src/mongo/db/modules/enterprise/jstests/encryptdb/libs:ekf",
        "//src/mongo/db/modules/enterprise/jstests/encryptdb/libs:ekf2",
        "//src/mongo/db/modules/enterprise/jstests/external_auth/lib:all_files",
        "//src/mongo/db/modules/enterprise/jstests/external_auth/lib:all_subpackage_javascript_files",
        "//src/mongo/db/modules/enterprise/jstests/external_auth_aws/lib:all_javascript_files",
        "//src/mongo/db/modules/enterprise/jstests/hot_backups/libs:all_javascript_files",
        "//src/mongo/db/modules/enterprise/jstests/live_restore/libs:all_javascript_files",
        "//src/third_party/schemastore.org:schemas",
        "//x509:generate_main_certificates",
    ]
    multiversion_config = ["//bazel/resmoke:multiversion_config"] if multiversion_deps else []

    # Each multiversion_setup target "last-lts" also produces a "last-lts_exclude_tags"
    multiversion_exclude_tags = [
        (dep.rsplit(":", 1)[0] + ":" + dep.rsplit(":", 1)[1] + "_exclude_tags") if ":" in dep else dep + "_exclude_tags"
        for dep in multiversion_deps
    ]

    # The list of tests this suite will run, for Evergreen's Test Selection Services (TSS).
    # Nothing consumes it yet. Discovery is given the same tag files as the test action below so
    # the list matches what the suite would really run.
    tss_test_list = name + "_tss_test_list"
    native.genrule(
        name = tss_test_list,
        srcs = [generated_config, "//bazel/resmoke:tss_settings"] + srcs + multiversion_exclude_tags,
        outs = [tss_test_list + ".yml"],
        cmd = ("$(location //bazel/resmoke:generate_tss_test_list) --suite=$(location {config}) " +
               "--label=//{pkg}:{name} --output=$@ --volatile-status=bazel-out/volatile-status.txt " +
               "--selection-settings=$(location //bazel/resmoke:tss_settings)").format(
            config = native.package_relative_label(generated_config),
            pkg = native.package_name(),
            name = name,
        ) + "".join([
            " --tagFile=$(location %s)" % native.package_relative_label(tag)
            for tag in multiversion_exclude_tags
        ]),
        stamp = True,
        tools = ["//bazel/resmoke:generate_tss_test_list"],
        # Must run on the Evergreen host, not a remote worker: a later step will call the
        # Test Selection Services API from here (required Mesh),
        # which needs the host's credentials and network. no-sandbox
        # because resmoke's selector resolves test files by globbing the working directory, so a
        # sandbox with incomplete inputs would yield a silently short list rather than an error.
        tags = ["no-remote", "external", "no-cache", "no-sandbox"],
    )

    # Data that is always safe for cquery (no C++ toolchain needed): auto-derived srcs,
    # default resmoke infrastructure, multiversion artifacts, the config fuzz seed file, and the
    # TSS test list. The test list is not read by the test yet, but it has to be reachable from
    # the test target to get built at all: CI builds with --build_tests_only, which skips
    # targets nothing depends on.
    cquery_safe_data = [d for d in srcs if d not in data] + [d for d in default_data if d not in data and d not in srcs] + multiversion_deps + multiversion_config + multiversion_exclude_tags + seed_target_data + [":" + tss_test_list]

    # If this suite's only multiversion dep is last-continuous, it is a
    # dedicated last-continuous suite.  Mark it incompatible when
    # last-continuous resolves to the same version as last-lts so that
    # `bazel test //...` skips it rather than running redundant tests.
    dep_names = [_dep_target_name(d) for d in multiversion_deps]
    if dep_names == ["last-continuous"]:
        target_compatible_with = target_compatible_with + select({
            "//bazel/resmoke/multiversion:last_continuous_redundant": ["@platforms//:incompatible"],
            "//conditions:default": [],
        })

    data_attr = select({
        # Skip user-provided data during cquery — it may include targets that require
        # C++ toolchain resolution, which fails when no_c++_toolchain=1 is set.
        "//bazel/resmoke:skip_deps_for_cquery_enabled": cquery_safe_data,
        "//conditions:default": data + cquery_safe_data,
    }) + select({
        "//bazel/resmoke:installed_dist_test_enabled": ["//:installed-dist-test"],
        "//conditions:default": [],
    })

    server_deps_attr = select({
        "//bazel/resmoke:installed_dist_test_enabled": [],
        "//bazel/resmoke:skip_deps_for_cquery_enabled": [],
        "//conditions:default": deps,
    })

    args_attr = [
        "run",
        "--suites=$(location %s)" % native.package_relative_label(generated_config),
        "--releasesFile=$(location //src/mongo/util/version:releases.yml)",
        "--archiveMode=directory",
        "--archiveLimitMb=500",
        "--testTimeout=$(RESMOKE_TEST_TIMEOUT)",
        "--historicTestRuntimes=$(location :%s)" % historic_runtimes,
        "--tssTestList=$(location :%s)" % tss_test_list,
        "--mongoVersionFile=$(location //bazel/resmoke:resmoke_mongo_version)",
    ] + [
        "--multiversionDir=$(location %s)" % native.package_relative_label(dep)
        for dep in multiversion_deps
    ] + [
        "--tagFile=$(location %s)" % native.package_relative_label(tag)
        for tag in multiversion_exclude_tags
    ] + extra_args + resmoke_args

    env_attr = {
        "LOCAL_RESOURCES": "$(LOCAL_RESOURCES)",
        "GIT_PYTHON_REFRESH": "quiet",  # Ignore "Bad git executable" error when importing git python. Git commands will still error if run.
        "PYTHON_IMPORTS_FILE": "$(location %s)" % native.package_relative_label(python_imports_target),
    } | seed_env | ({
        "MULTIVERSION_CONFIG_FILE": "$(location //bazel/resmoke:multiversion_config)",
        "MULTIVERSION_VERSIONS": ",".join([
            dep.rsplit(":", 1)[1] if ":" in dep else dep
            for dep in multiversion_deps
        ]),
    } if multiversion_deps else {})

    # The resmoke Python program. Not run directly (the _resmoke_test wrapper execs
    # it), so it is tagged manual.
    py_binary(
        name = name + "_bin",
        srcs = [resmoke_shim],
        data = data_attr + select({
            "//bazel/resmoke:installed_dist_test_enabled": [],
            "//bazel/resmoke:skip_deps_for_cquery_enabled": [],
            "//conditions:default": [":%s" % deps_path_file],
        }),
        deps = [
            resmoke,
            "//buildscripts:bazel_local_resources",
        ],
        main = resmoke_shim,
        tags = ["manual"],
        target_compatible_with = target_compatible_with,
    )

    # Bazel caps the native shard_count attribute at 50. Above that, pin the attribute at the cap
    # and let the rule transition force the real count, so a suite still shards 50 ways if the
    # transition is disabled (--no//bazel/resmoke:support_extended_shard_count) rather than collapsing to a
    # single shard.
    shard_count = kwargs.pop("shard_count", None)
    forced_shards = 0
    if shard_count != None and shard_count > _MAX_NATIVE_SHARD_COUNT:
        forced_shards = shard_count
        shard_count = _MAX_NATIVE_SHARD_COUNT
    if shard_count != None:
        kwargs["shard_count"] = shard_count

    _resmoke_test(
        name = name,
        forced_shards = forced_shards,
        resmoke_bin = ":" + name + "_bin",
        server_deps = server_deps_attr,
        data = data_attr,
        resmoke_args = args_attr,
        resmoke_env = env_attr,
        tags = tags + ["no-cache", "resources:port_block:1", "resmoke_suite_test"],
        target_compatible_with = target_compatible_with,
        timeout = timeout,
        exec_properties = exec_properties | test_exec_properties(kwargs.get("size", "medium")),  # medium is the bazel default size for all tests
        toolchains = [
            "//bazel/resmoke:test_timeout",
        ],
        **kwargs
    )

# ---------------------------------------------------------------------------
# _resmoke_test rule (private; the public entry point is the resmoke_suite_test macro).
#
# This is essentially a reimplementation of py_test that wraps a resmoke py_binary,
# but allows us to customize with features that py_test doesn't natively support or
# can extend (eg. collect code coverage).
#
# A resmoke suite is a Python program (the resmoke shim) that boots mongod/mongos
# subprocesses. This rule wraps the resmoke py_binary in a small launcher that
# reproduces the args/env a py_test applies (with $(location)/make-variable
# expansion), so the target behaves the same as a py_test.
# ---------------------------------------------------------------------------
def _resmoke_test_impl(ctx):
    resmoke_bin = ctx.executable.resmoke_bin

    # Expand $(location ...) against every target that can be referenced, and
    # make-variables ($(RESMOKE_TEST_TIMEOUT) from the test_timeout toolchain,
    # $(LOCAL_RESOURCES) from a --define) via ctx.var.
    expansion_targets = ctx.attr.data + ctx.attr.server_deps + [ctx.attr.resmoke_bin]

    def expand(s):
        # Native py_test expands $(location) in args/env to the runfiles-relative
        # (rootpath) form, but ctx.expand_location maps $(location) to the exec path.
        # Rewrite to $(rootpath) so paths resolve the same way resmoke expects.
        s = s.replace("$(location ", "$(rootpath ").replace("$(locations ", "$(rootpaths ")
        s = ctx.expand_location(s, expansion_targets)
        return ctx.expand_make_variables("resmoke_test", s, ctx.var)

    def sh_quote(s):
        # Wrap in single quotes, escaping embedded single quotes.
        return "'" + s.replace("'", "'\\''") + "'"

    expanded_args = [expand(a) for a in ctx.attr.resmoke_args]
    expanded_env = {k: expand(v) for k, v in ctx.attr.resmoke_env.items()}

    # resmoke locates the server binaries (mongod/mongos/...) via DEPS_PATH. Resolve
    # each server_dep to its executable rather than a $(rootpath): a binary built with
    # separate debug info produces multiple output files (e.g. mongos + mongos.debug),
    # which the singular $(rootpath) rejects. The native py_test this rule replaced
    # resolved such a label to the target's executable, so we reproduce that here.
    server_bins = [dep[DefaultInfo].files_to_run.executable for dep in ctx.attr.server_deps]
    if server_bins:
        expanded_env["DEPS_PATH"] = ":".join([b.short_path for b in server_bins])

    # Bazel only auto-populates the C++ coverage collector env (used by
    # collect_cc_coverage.sh) when the test's own executable is a cc-instrumented
    # binary. Our launcher is a shell script, so set it explicitly from the resolved
    # cc toolchain. Under coverage, the clang toolchain maps the "gcov" tool to
    # llvm-profdata; llvm-cov is its sibling in the same bin dir.
    cc_toolchain = find_cc_toolchain(ctx, mandatory = False)

    # Note that gcov_executable can be used for either gcov or llvm-profdata
    gcov = cc_toolchain.gcov_executable if cc_toolchain else None
    if gcov:
        bindir = gcov.rsplit("/", 1)[0]

        # LLVM source-based coverage (.profraw): COVERAGE_GCOV_PATH is the merge tool
        # (the clang toolchain maps the "gcov" tool_path to llvm-profdata) and LLVM_COV
        # does the lcov export.
        expanded_env["COVERAGE_GCOV_PATH"] = gcov
        expanded_env["LLVM_COV"] = bindir + "/llvm-cov"

    env_exports = "".join(
        ["export {}={}\n".format(k, sh_quote(v)) for k, v in expanded_env.items()],
    )

    # Route each instrumented server process's counters into COVERAGE_DIR using
    # continuous mode (%c) + per-module (%m) online merging. Continuous mode mmaps
    # counters live so they persist across quickExit()'s _exit() (which skips the
    # profile runtime's atexit flush); %m gives each DSO its own file and enables the
    # merge pool across concurrent processes. COVERAGE_DIR is exported at runtime by
    # Bazel's collect_coverage.sh, so the value must expand there (not at analysis time).
    profile_override = (
        'if [[ -n "${COVERAGE_DIR:-}" ]]; then\n' +
        '  export LLVM_PROFILE_FILE="${COVERAGE_DIR}/server-%p-%m%c.profraw"\n' +
        "fi\n"
    )

    args_str = " ".join([sh_quote(a) for a in expanded_args])

    # The launcher locates the resmoke py_binary under the runfiles tree and execs it.
    # bazel test (and RBE) materialize the runfiles tree and set RUNFILES_DIR; if
    # neither RUNFILES_DIR nor a manifest is set we fall back to the adjacent
    # <launcher>.runfiles directory.
    launcher = ctx.actions.declare_file(ctx.label.name + "_launcher.sh")
    ctx.actions.write(
        output = launcher,
        is_executable = True,
        content = """#!/usr/bin/env bash
set -euo pipefail
if [[ -z "${{RUNFILES_DIR:-}}" && -z "${{RUNFILES_MANIFEST_FILE:-}}" ]]; then
  if [[ -d "$0.runfiles" ]]; then
    export RUNFILES_DIR="$0.runfiles"
  fi
fi
{env_exports}{profile_override}exec "{resmoke_bin}" {args} "$@"
""".format(
            env_exports = env_exports,
            profile_override = profile_override,
            # The py_binary executable lives in the runfiles under _main/<short_path>.
            resmoke_bin = "${RUNFILES_DIR}/_main/" + resmoke_bin.short_path,
            args = args_str,
        ),
    )

    # Seed with the data files and the server executables that DEPS_PATH points at, so
    # those exact binaries are staged rather than relying on each dep's default_runfiles
    # to carry its own executable.
    runfiles = ctx.runfiles(
        files = ctx.files.data + server_bins,
        transitive_files = cc_toolchain.all_files if cc_toolchain and ctx.configuration.coverage_enabled else None,
    )
    runfiles = runfiles.merge(ctx.attr.resmoke_bin[DefaultInfo].default_runfiles)
    for dep in ctx.attr.server_deps + ctx.attr.data:
        runfiles = runfiles.merge(dep[DefaultInfo].default_runfiles)

    return [
        DefaultInfo(executable = launcher, runfiles = runfiles),
        RunEnvironmentInfo(environment = expanded_env),
        # Forward coverage info from the instrumented server binaries so that the
        # coverage manifest lists their objects and sources.
        coverage_common.instrumented_files_info(
            ctx,
            dependency_attributes = ["server_deps", "data"],
        ),
    ]

# Scope the llvm_coverage_dso feature (see bazel/toolchains/cc/mongo_custom_features.bzl)
# to the server binaries' build subgraph, and only when coverage is being collected.
# Enabling it globally (via `.bazelrc coverage --features=`) would add
# -runtime-counter-relocation to every instrumented TU in the build — including
# unittests, whose coverage runs must stay as fast as possible. The feature is only
# needed so mongod's quickExit()->_exit() flushes counters; unittests exit normally
# and don't need it. A configuration transition (rather than a target `features`
# attribute) is required because the counters live in the transitively-linked
# cc_library TUs, which a cc_binary-level `features` attr would not reach.
def _coverage_dso_transition_impl(settings, _attr):
    if not settings["//command_line_option:collect_code_coverage"]:
        return {}
    features = list(settings["//command_line_option:features"])
    if "llvm_coverage_dso" not in features:
        features.append("llvm_coverage_dso")
    return {"//command_line_option:features": features}

_coverage_dso_transition = transition(
    implementation = _coverage_dso_transition_impl,
    inputs = [
        "//command_line_option:collect_code_coverage",
        "//command_line_option:features",
    ],
    outputs = ["//command_line_option:features"],
)

# Bazel rejects a native shard_count above 50. Suites that need more shards than that get their
# shard count from --test_sharding_strategy instead, set here per target so it applies to this
# suite alone rather than to every test in the invocation.
def _forced_shards_transition_impl(settings, attr):
    if attr.forced_shards == 0:
        return {}
    if not settings["//bazel/resmoke:support_extended_shard_count"]:
        return {}
    return {"//command_line_option:test_sharding_strategy": "forced=%d" % attr.forced_shards}

_forced_shards_transition = transition(
    implementation = _forced_shards_transition_impl,
    inputs = ["//bazel/resmoke:support_extended_shard_count"],
    outputs = ["//command_line_option:test_sharding_strategy"],
)

_resmoke_test = rule(
    implementation = _resmoke_test_impl,
    cfg = _forced_shards_transition,
    attrs = {
        "forced_shards": attr.int(
            doc = "Set by the resmoke_suite_test macro when shard_count exceeds the native cap; " +
                  "forces this many shards via a configuration transition. 0 leaves sharding to " +
                  "the native shard_count attribute.",
        ),
        "resmoke_bin": attr.label(
            mandatory = True,
            executable = True,
            cfg = "target",
            doc = "The resmoke py_binary to run.",
        ),
        "server_deps": attr.label_list(
            cfg = _coverage_dso_transition,
            doc = "Server binaries (mongod/mongos/mongo). Under coverage, built with the " +
                  "llvm_coverage_dso feature so each DSO flushes its counters across quickExit().",
        ),
        "data": attr.label_list(
            allow_files = True,
            doc = "Runtime data dependencies (configs, jstests, certs).",
        ),
        "resmoke_args": attr.string_list(
            doc = "Arguments passed to resmoke; support $(location) and make-vars.",
        ),
        "resmoke_env": attr.string_dict(
            doc = "Environment for the test; values support $(location) and make-vars.",
        ),
        # C++ coverage plumbing (see extract_debuginfo_test).
        "_cc_toolchain": attr.label(default = "@rules_cc//cc:optional_current_cc_toolchain"),
        "_lcov_merger": attr.label(
            default = configuration_field(fragment = "coverage", name = "output_generator"),
            executable = True,
            cfg = config.exec(exec_group = "test"),
        ),
        "_collect_cc_coverage": attr.label(
            default = "@bazel_tools//tools/test:collect_cc_coverage",
            executable = True,
            cfg = config.exec(exec_group = "test"),
        ),
    },
    toolchains = use_cc_toolchain(mandatory = False),
    fragments = ["cpp", "coverage"],
    test = True,
)
