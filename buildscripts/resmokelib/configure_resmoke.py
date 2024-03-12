"""Configure the command line input for the resmoke 'run' subcommand."""

import argparse
import collections
import configparser
import datetime
import os
import os.path
from pathlib import Path
import sys
import platform
import random
import glob
import textwrap
import shlex
import shutil
import traceback
from typing import Dict, Optional

import pymongo.uri_parser
import yaml
from opentelemetry import trace, context, baggage
from opentelemetry.trace import NonRecordingSpan, SpanContext, TraceFlags
from opentelemetry.sdk.resources import SERVICE_NAME, Resource
from opentelemetry.sdk.trace import TracerProvider
from buildscripts.resmokelib.utils.batched_baggage_span_processor import BatchedBaggageSpanProcessor
from buildscripts.resmokelib.utils.file_span_exporter import FileSpanExporter

from buildscripts.idl import gen_all_feature_flag_list

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import utils
from buildscripts.resmokelib import mongo_fuzzer_configs
from buildscripts.util.read_config import read_config_file

BASE_16_TO_INT = 16


def validate_and_update_config(parser, args):
    """Validate inputs and update config module."""
    _validate_options(parser, args)
    _update_config_vars(parser, args)
    _update_symbolizer_secrets()
    _validate_config(parser)
    _set_logging_config()


def _validate_options(parser, args):
    """Do preliminary validation on the options and error on any invalid options."""

    if not 'shell_port' in args or not 'shell_conn_string' in args:
        return

    if args.shell_port is not None and args.shell_conn_string is not None:
        parser.error("Cannot specify both `shellPort` and `shellConnString`")

    if args.executor_file:
        parser.error("--executor is superseded by --suites; specify --suites={} {} to run the"
                     " test(s) under those suite configuration(s)".format(
                         args.executor_file, " ".join(args.test_files)))

    # The "test_files" positional argument logically overlaps with `--replayFile`. Disallow using both.
    if args.test_files and args.replay_file:
        parser.error(
            "Cannot use --replayFile with additional test files listed on the command line invocation."
        )

    if args.shell_seed and (not args.test_files or len(args.test_files) != 1):
        parser.error("The --shellSeed argument must be used with only one test.")

    if args.additional_feature_flags_file and not os.path.isfile(
            args.additional_feature_flags_file):
        parser.error("The specified additional feature flags file does not exist.")

    def get_set_param_errors(process_params):
        agg_set_params = collections.defaultdict(list)
        for set_param in process_params:
            for key, value in utils.load_yaml(set_param).items():
                agg_set_params[key] += [value]

        errors = []
        for key, values in agg_set_params.items():
            if len(values) == 1:
                continue

            for left, _ in enumerate(values):
                for right in range(left + 1, len(values)):
                    if values[left] != values[right]:
                        errors.append(
                            f"setParameter has multiple distinct values. Key: {key} Values: {values}"
                        )

        return errors

    config = vars(args)
    mongod_set_param_errors = get_set_param_errors(config.get('mongod_set_parameters') or [])
    mongos_set_param_errors = get_set_param_errors(config.get('mongos_set_parameters') or [])
    mongocryptd_set_param_errors = get_set_param_errors(
        config.get('mongocryptd_set_parameters') or [])
    error_msgs = {}
    if mongod_set_param_errors:
        error_msgs["mongodSetParameters"] = mongod_set_param_errors
    if mongos_set_param_errors:
        error_msgs["mongosSetParameters"] = mongos_set_param_errors
    if mongocryptd_set_param_errors:
        error_msgs["mongocryptdSetParameters"] = mongocryptd_set_param_errors
    if error_msgs:
        parser.error(str(error_msgs))


def _validate_config(parser):
    """Do validation on the config settings."""

    if _config.REPEAT_TESTS_MAX:
        if not _config.REPEAT_TESTS_SECS:
            parser.error("Must specify --repeatTestsSecs with --repeatTestsMax")

        if _config.REPEAT_TESTS_MIN > _config.REPEAT_TESTS_MAX:
            parser.error("--repeatTestsSecsMin > --repeatTestsMax")

    if _config.REPEAT_TESTS_MIN and not _config.REPEAT_TESTS_SECS:
        parser.error("Must specify --repeatTestsSecs with --repeatTestsMin")

    if _config.REPEAT_TESTS > 1 and _config.REPEAT_TESTS_SECS:
        parser.error("Cannot specify --repeatTests and --repeatTestsSecs")

    if _config.MIXED_BIN_VERSIONS is not None:
        for version in _config.MIXED_BIN_VERSIONS:
            if version not in set(['old', 'new']):
                parser.error("Must specify binary versions as 'old' or 'new' in format"
                             " 'version1-version2'")

    if _config.UNDO_RECORDER_PATH is not None:
        if not sys.platform.startswith('linux') or platform.machine() not in [
                "i386", "i686", "x86_64"
        ]:
            parser.error("--recordWith is only supported on x86 and x86_64 Linux distributions")
            return

        resolved_path = shutil.which(_config.UNDO_RECORDER_PATH)
        if resolved_path is None:
            parser.error(
                f"Cannot find the UndoDB live-record binary '{_config.UNDO_RECORDER_PATH}'. Check that it exists and is executable"
            )
            return

        if not os.access(resolved_path, os.X_OK):
            parser.error(f"Found '{resolved_path}', but it is not an executable file")

    if not _config.TLS_MODE or _config.TLS_MODE == "disabled":
        if _config.SHELL_TLS_ENABLED:
            parser.error("--shellTls requires server TLS to be enabled")
        if _config.TLS_CA_FILE:
            parser.error("--tlsCAFile requires server TLS to be enabled")
        if _config.MONGOD_TLS_CERTIFICATE_KEY_FILE:
            parser.error("--mongodTlsCertificateKeyFile requires server TLS to be enabled")
        if _config.MONGOS_TLS_CERTIFICATE_KEY_FILE:
            parser.error("--mongosTlsCertificateKeyFile requires server TLS to be enabled")

    if not _config.SHELL_TLS_ENABLED:
        if _config.SHELL_TLS_CERTIFICATE_KEY_FILE:
            parser.error("--shellTlsCertificateKeyFile requires --shellTls")


def _find_resmoke_wrappers():
    # This is technically incorrect. PREFIX_BINDIR defaults to $PREFIX/bin, so
    # if the user changes it to any some other value, this glob will fail to
    # detect the resmoke wrapper.
    # Additionally, the resmoke wrapper will not be found if a user performs
    # their builds outside of the git repository root, (ex checkout at
    # /data/mongo, build-dir at /data/build)
    # We assume that users who fall under either case will explicitly pass the
    # --installDir argument.
    candidate_installs = glob.glob("**/bin/resmoke.py", recursive=True)
    return list(candidate_installs)


def _set_up_tracing(
        otel_collector_dir: Optional[str],
        trace_id: Optional[str],
        parent_span_id: Optional[str],
        extra_context: Optional[Dict[str, object]],
) -> bool:
    """Try to set up otel tracing. On success return True. On failure return False.

    This method does 3 things:
    1. If a user passes in a directory pathname to store OTel metrics in,
    then we export metrics to files in that directory using our custom exporter
    `FileSpanExporter`.
    2. If a user passes in a trace ID and a parent span ID, we assume both of those.
    This allows us to tie resmoke metrics to a parent metric.
    3. If a user passes in extra_context, we add these "global" values to our baggage.
    This allows us to propagate these values to all child spans in resmoke
    using our custom span processor `BatchedBaggageSpanProcessor`.
    """

    success = True
    # Service name is required for most backends
    resource = Resource(attributes={SERVICE_NAME: "resmoke"})

    provider = TracerProvider(resource=resource)
    if otel_collector_dir:
        try:
            otel_collector_dir = Path(otel_collector_dir)
            otel_collector_dir.mkdir(parents=True, exist_ok=True)
            # Make the file easy to read when ran locally.
            pretty_print = _config.EVERGREEN_TASK_ID is None
            processor = BatchedBaggageSpanProcessor(
                FileSpanExporter(otel_collector_dir, pretty_print))
            provider.add_span_processor(processor)
        except OSError:
            traceback.print_exc()
            success = False
    trace.set_tracer_provider(provider)

    if trace_id and parent_span_id:
        span_context = SpanContext(
            trace_id=int(trace_id, BASE_16_TO_INT),
            span_id=int(parent_span_id, BASE_16_TO_INT),
            is_remote=False,
            # Magic number needed for our OTEL collector
            trace_flags=TraceFlags(0x01),
        )
        ctx = trace.set_span_in_context(NonRecordingSpan(span_context))
        context.attach(ctx)

    if extra_context:
        for key, value in extra_context.items():
            if value is not None:
                context.attach(baggage.set_baggage(key, value))
    return success


def _update_config_vars(parser, values):
    """Update the variables of the config module."""

    config = _config.DEFAULTS.copy()

    # Override `config` with values from command line arguments.
    cmdline_vars = vars(values)
    for cmdline_key in cmdline_vars:
        if cmdline_key not in _config.DEFAULTS:
            # Ignore options that don't map to values in config.py
            continue
        if cmdline_vars[cmdline_key] is not None:
            config[cmdline_key] = cmdline_vars[cmdline_key]

    if values.command == "run" and os.path.isfile("resmoke.ini"):
        err = textwrap.dedent("""\
Support for resmoke.ini has been removed. You must delete
resmoke.ini and rerun your build to run resmoke. If only one testable
installation is present, resmoke will automatically locate that installation.
If you have multiple installations, you must either pass an explicit
--installDir argument to the run subcommand to identify the installation you
would like to test, or invoke the customized resmoke.py wrapper script staged
into the bin directory of each installation.""")
        config_parser = configparser.ConfigParser()
        config_parser.read("resmoke.ini")
        if "resmoke" in config_parser.sections():
            user_config = dict(config_parser["resmoke"])
            err += textwrap.dedent(f"""

Based on the current value of resmoke.ini, after rebuilding, resmoke.py should
be invoked as either:
- {shlex.quote(f"{user_config['install_dir']}/resmoke.py")}
- buildscripts/resmoke.py --installDir {shlex.quote(user_config['install_dir'])}""")
        raise RuntimeError(err)

    def process_feature_flag_file(path):
        with open(path) as fd:
            return fd.read().split()

    def setup_feature_flags():
        _config.RUN_ALL_FEATURE_FLAG_TESTS = config.pop("run_all_feature_flag_tests")
        _config.RUN_NO_FEATURE_FLAG_TESTS = config.pop("run_no_feature_flag_tests")
        _config.ADDITIONAL_FEATURE_FLAGS_FILE = config.pop("additional_feature_flags_file")

        if values.command == "run":
            # These logging messages start with # becuase the output of this file must produce
            # valid yaml. This comments out these print statements when the output is parsed.
            print("# Fetching feature flags...")
            all_ff = gen_all_feature_flag_list.gen_all_feature_flags()
            print("# Fetched feature flags...")
        else:
            all_ff = []

        enabled_feature_flags = []
        if _config.RUN_ALL_FEATURE_FLAG_TESTS:
            enabled_feature_flags = all_ff[:]

        if _config.ADDITIONAL_FEATURE_FLAGS_FILE:
            enabled_feature_flags.extend(
                process_feature_flag_file(_config.ADDITIONAL_FEATURE_FLAGS_FILE))

        # Specify additional feature flags from the command line.
        # Set running all feature flag tests to True if this options is specified.
        additional_feature_flags = _tags_from_list(config.pop("additional_feature_flags"))
        if additional_feature_flags is not None:
            enabled_feature_flags.extend(additional_feature_flags)

        return enabled_feature_flags, all_ff

    _config.ENABLED_FEATURE_FLAGS, all_feature_flags = setup_feature_flags()
    not_enabled_feature_flags = list(set(all_feature_flags) - set(_config.ENABLED_FEATURE_FLAGS))

    _config.AUTO_KILL = config.pop("auto_kill")
    _config.ALWAYS_USE_LOG_FILES = config.pop("always_use_log_files")
    _config.BASE_PORT = int(config.pop("base_port"))
    _config.BACKUP_ON_RESTART_DIR = config.pop("backup_on_restart_dir")
    _config.BUILDLOGGER_URL = config.pop("buildlogger_url")
    _config.DBPATH_PREFIX = _expand_user(config.pop("dbpath_prefix"))
    _config.DRY_RUN = config.pop("dry_run")

    # EXCLUDE_WITH_ANY_TAGS will always contain the implicitly defined EXCLUDED_TAG.
    _config.EXCLUDE_WITH_ANY_TAGS = [_config.EXCLUDED_TAG]
    _config.EXCLUDE_WITH_ANY_TAGS.extend(
        utils.default_if_none(_tags_from_list(config.pop("exclude_with_any_tags")), []))

    with open("buildscripts/resmokeconfig/fully_disabled_feature_flags.yml") as fully_disabled_ffs:
        force_disabled_flags = yaml.safe_load(fully_disabled_ffs)

    _config.EXCLUDE_WITH_ANY_TAGS.extend(force_disabled_flags)

    if _config.RUN_NO_FEATURE_FLAG_TESTS:
        # Don't run any feature flag tests.
        _config.EXCLUDE_WITH_ANY_TAGS.extend(all_feature_flags)
    else:
        # Don't run tests with feature flags that are not enabled.
        _config.EXCLUDE_WITH_ANY_TAGS.extend(not_enabled_feature_flags)

    _config.FAIL_FAST = not config.pop("continue_on_failure")
    _config.FLOW_CONTROL = config.pop("flow_control")
    _config.FLOW_CONTROL_TICKETS = config.pop("flow_control_tickets")

    _config.INCLUDE_WITH_ANY_TAGS = _tags_from_list(config.pop("include_with_any_tags"))
    _config.INCLUDE_TAGS = _tags_from_list(config.pop("include_with_all_tags"))

    _config.GENNY_EXECUTABLE = _expand_user(config.pop("genny_executable"))
    _config.JOBS = config.pop("jobs")
    _config.LINEAR_CHAIN = config.pop("linear_chain") == "on"
    _config.MAJORITY_READ_CONCERN = config.pop("majority_read_concern") == "on"
    _config.ENABLE_ENTERPRISE_TESTS = config.pop("enable_enterprise_tests")
    _config.MIXED_BIN_VERSIONS = config.pop("mixed_bin_versions")
    if _config.MIXED_BIN_VERSIONS is not None:
        _config.MIXED_BIN_VERSIONS = _config.MIXED_BIN_VERSIONS.split("-")

    _config.MULTIVERSION_BIN_VERSION = config.pop("old_bin_version")

    _config.INSTALL_DIR = config.pop("install_dir")
    if values.command == "run" and _config.INSTALL_DIR is None:
        resmoke_wrappers = _find_resmoke_wrappers()
        if len(resmoke_wrappers) == 1:
            _config.INSTALL_DIR = os.path.dirname(resmoke_wrappers[0])
        elif len(resmoke_wrappers) > 1:
            err = textwrap.dedent(f"""\
Multiple testable installations were found, but installDir was not specified.
You must either call resmoke via one of the following scripts:
{os.linesep.join(map(shlex.quote, resmoke_wrappers))}

or explicitly pass --installDir to the run subcommand of buildscripts/resmoke.py.""")
            raise RuntimeError(err)
    if _config.INSTALL_DIR is not None:
        # Normalize the path so that on Windows dist-test/bin
        # translates to .\dist-test\bin then absolutify it since the
        # Windows PATH variable requires absolute paths.
        _config.INSTALL_DIR = os.path.abspath(_expand_user(os.path.normpath(_config.INSTALL_DIR)))

        for binary in ["mongo", "mongod", "mongos", "mongot-localdev/mongot", "dbtest"]:
            keyname = binary + "_executable"
            if config.get(keyname, None) is None:
                config[keyname] = os.path.join(_config.INSTALL_DIR, binary)

    _config.DBTEST_EXECUTABLE = _expand_user(config.pop("dbtest_executable"))
    _config.MONGO_EXECUTABLE = _expand_user(config.pop("mongo_executable"))

    def _merge_set_params(param_list):
        ret = {}
        for set_param in param_list:
            ret.update(utils.load_yaml(set_param))
        return utils.dump_yaml(ret)

    _config.MONGOD_EXECUTABLE = _expand_user(config.pop("mongod_executable"))

    mongod_set_parameters = config.pop("mongod_set_parameters")

    _config.MONGOD_SET_PARAMETERS = _merge_set_params(mongod_set_parameters)
    _config.FUZZ_MONGOD_CONFIGS = config.pop("fuzz_mongod_configs")
    _config.FUZZ_MONGOS_CONFIGS = config.pop("fuzz_mongos_configs")
    _config.CONFIG_FUZZ_SEED = config.pop("config_fuzz_seed")

    if _config.FUZZ_MONGOD_CONFIGS:
        if not _config.CONFIG_FUZZ_SEED:
            _config.CONFIG_FUZZ_SEED = random.randrange(sys.maxsize)
        else:
            _config.CONFIG_FUZZ_SEED = int(_config.CONFIG_FUZZ_SEED)
        _config.MONGOD_SET_PARAMETERS, _config.WT_ENGINE_CONFIG, _config.WT_COLL_CONFIG, \
        _config.WT_INDEX_CONFIG = mongo_fuzzer_configs.fuzz_mongod_set_parameters(
            _config.FUZZ_MONGOD_CONFIGS, _config.CONFIG_FUZZ_SEED, _config.MONGOD_SET_PARAMETERS)
        _config.EXCLUDE_WITH_ANY_TAGS.extend(["uses_compact"])
        _config.EXCLUDE_WITH_ANY_TAGS.extend(["requires_emptycapped"])

    _config.MONGOS_EXECUTABLE = _expand_user(config.pop("mongos_executable"))
    mongos_set_parameters = config.pop("mongos_set_parameters")
    _config.MONGOS_SET_PARAMETERS = _merge_set_params(mongos_set_parameters)

    if _config.FUZZ_MONGOS_CONFIGS:
        if not _config.CONFIG_FUZZ_SEED:
            _config.CONFIG_FUZZ_SEED = random.randrange(sys.maxsize)
        else:
            _config.CONFIG_FUZZ_SEED = int(_config.CONFIG_FUZZ_SEED)

        _config.MONGOS_SET_PARAMETERS = mongo_fuzzer_configs.fuzz_mongos_set_parameters(
            _config.FUZZ_MONGOS_CONFIGS, _config.CONFIG_FUZZ_SEED, _config.MONGOS_SET_PARAMETERS)

    _config.MONGOCRYPTD_SET_PARAMETERS = _merge_set_params(config.pop("mongocryptd_set_parameters"))

    _config.MONGOT_EXECUTABLE = _expand_user(config.pop("mongot-localdev/mongot_executable"))
    mongot_set_parameters = config.pop("mongot_set_parameters")
    _config.MONGOT_SET_PARAMETERS = _merge_set_params(mongot_set_parameters)

    _config.MRLOG = config.pop("mrlog")
    _config.NO_JOURNAL = config.pop("no_journal")
    _config.NUM_CLIENTS_PER_FIXTURE = config.pop("num_clients_per_fixture")
    _config.USE_TENANT_CLIENT = config.pop("use_tenant_client")
    _config.NUM_REPLSET_NODES = config.pop("num_replset_nodes")
    _config.TLS_MODE = config.pop("tls_mode")
    _config.TLS_CA_FILE = config.pop("tls_ca_file")
    _config.SHELL_TLS_ENABLED = config.pop("shell_tls_enabled")
    _config.SHELL_TLS_CERTIFICATE_KEY_FILE = config.pop("shell_tls_certificate_key_file")
    _config.SHELL_GRPC = config.pop("shell_grpc")
    _config.MONGOD_TLS_CERTIFICATE_KEY_FILE = config.pop("mongod_tls_certificate_key_file")
    _config.MONGOS_TLS_CERTIFICATE_KEY_FILE = config.pop("mongos_tls_certificate_key_file")
    _config.NUM_SHARDS = config.pop("num_shards")
    _config.CONFIG_SHARD = utils.pick_catalog_shard_node(
        config.pop("config_shard"), _config.NUM_SHARDS)
    _config.EMBEDDED_ROUTER = config.pop("embedded_router")
    _config.ORIGIN_SUITE = config.pop("origin_suite")
    _config.PERF_REPORT_FILE = config.pop("perf_report_file")
    _config.CEDAR_REPORT_FILE = config.pop("cedar_report_file")
    _config.RANDOM_SEED = config.pop("seed")
    _config.REPEAT_SUITES = config.pop("repeat_suites")
    _config.REPEAT_TESTS = config.pop("repeat_tests")
    _config.REPEAT_TESTS_MAX = config.pop("repeat_tests_max")
    _config.REPEAT_TESTS_MIN = config.pop("repeat_tests_min")
    _config.REPEAT_TESTS_SECS = config.pop("repeat_tests_secs")
    _config.REPORT_FILE = config.pop("report_file")
    _config.SERVICE_EXECUTOR = config.pop("service_executor")
    _config.EXPORT_MONGOD_CONFIG = config.pop("export_mongod_config")
    _config.SHELL_SEED = config.pop("shell_seed")
    _config.STAGGER_JOBS = config.pop("stagger_jobs") == "on"
    _config.STORAGE_ENGINE = config.pop("storage_engine")
    _config.STORAGE_ENGINE_CACHE_SIZE = config.pop("storage_engine_cache_size_gb")
    _config.SUITE_FILES = config.pop("suite_files")
    if _config.SUITE_FILES is not None:
        _config.SUITE_FILES = _config.SUITE_FILES.split(",")
    _config.TAG_FILES = config.pop("tag_files")
    _config.USER_FRIENDLY_OUTPUT = config.pop("user_friendly_output")
    _config.SANITY_CHECK = config.pop("sanity_check")
    _config.DOCKER_COMPOSE_BUILD_IMAGES = config.pop("docker_compose_build_images")
    if _config.DOCKER_COMPOSE_BUILD_IMAGES is not None:
        _config.DOCKER_COMPOSE_BUILD_IMAGES = _config.DOCKER_COMPOSE_BUILD_IMAGES.split(",")
    _config.DOCKER_COMPOSE_BUILD_ENV = config.pop("docker_compose_build_env")
    _config.DOCKER_COMPOSE_TAG = config.pop("docker_compose_tag")
    _config.EXTERNAL_SUT = config.pop("external_sut")

    # This is set to True when:
    # (1) We are building images for an External SUT, OR ...
    # (2) We are running resmoke against an External SUT
    _config.NOOP_MONGO_D_S_PROCESSES = _config.DOCKER_COMPOSE_BUILD_IMAGES is not None or _config.EXTERNAL_SUT

    # When running resmoke against an External SUT, we are expected to be in
    # the workload container -- which may require additional setup before running tests.
    _config.REQUIRES_WORKLOAD_CONTAINER_SETUP = _config.EXTERNAL_SUT

    # Internal testing options.
    _config.INTERNAL_PARAMS = config.pop("internal_params")

    # Evergreen options.
    _config.EVERGREEN_URL = config.pop("evergreen_url")
    _config.EVERGREEN_BUILD_ID = config.pop("build_id")
    _config.EVERGREEN_DISTRO_ID = config.pop("distro_id")
    _config.EVERGREEN_EXECUTION = config.pop("execution_number")
    _config.EVERGREEN_PATCH_BUILD = config.pop("patch_build")
    _config.EVERGREEN_PROJECT_NAME = config.pop("project_name")
    _config.EVERGREEN_REVISION = config.pop("git_revision")
    _config.EVERGREEN_REVISION_ORDER_ID = config.pop("revision_order_id")
    _config.EVERGREEN_TASK_ID = config.pop("task_id")
    _config.EVERGREEN_TASK_NAME = config.pop("task_name")
    _config.EVERGREEN_TASK_DOC = config.pop("task_doc")
    _config.EVERGREEN_VARIANT_NAME = config.pop("variant_name")
    _config.EVERGREEN_VERSION_ID = config.pop("version_id")
    _config.EVERGREEN_WORK_DIR = config.pop("work_dir")
    _config.EVERGREEN_PROJECT_CONFIG_PATH = config.pop("evg_project_config_path")

    # otel info
    _config.OTEL_TRACE_ID = config.pop("otel_trace_id")
    _config.OTEL_PARENT_ID = config.pop("otel_parent_id")
    _config.OTEL_COLLECTOR_DIR = config.pop("otel_collector_dir")

    try:
        setup_success = _set_up_tracing(
            _config.OTEL_COLLECTOR_DIR,
            _config.OTEL_TRACE_ID,
            _config.OTEL_PARENT_ID,
            extra_context={
                "evergreen.build.id": _config.EVERGREEN_BUILD_ID,
                "evergreen.distro.id": _config.EVERGREEN_DISTRO_ID,
                "evergreen.project.identifier": _config.EVERGREEN_PROJECT_NAME,
                "evergreen.task.execution": _config.EVERGREEN_EXECUTION,
                "evergreen.task.id": _config.EVERGREEN_TASK_ID,
                "evergreen.task.name": _config.EVERGREEN_TASK_NAME,
                "evergreen.variant.name": _config.EVERGREEN_VARIANT_NAME,
                "evergreen.revision": _config.EVERGREEN_REVISION,
                "evergreen.patch_build": _config.EVERGREEN_PATCH_BUILD,
            },
        )
        if not setup_success:
            print("Failed to create file to send otel metrics to. Continuing.")
    except:
        # We want this as a catch all exception
        # If there is some problem setting up metrics we don't want resmoke to fail
        # We would rather just swallow the error
        traceback.print_exc()
        print("Failed to set up otel metrics. Continuing.")

    # Force invalid suite config
    _config.FORCE_EXCLUDED_TESTS = config.pop("force_excluded_tests")

    # Archival options. Archival is enabled only when running on evergreen.
    if not _config.EVERGREEN_TASK_ID:
        _config.ARCHIVE_FILE = None
    else:
        # Enable archival globally for all mainline variants.
        if _config.EVERGREEN_VARIANT_NAME is not None and not _config.EVERGREEN_PATCH_BUILD:
            _config.FORCE_ARCHIVE_ALL_DATA_FILES = True

    _config.ARCHIVE_LIMIT_MB = config.pop("archive_limit_mb")
    _config.ARCHIVE_LIMIT_TESTS = config.pop("archive_limit_tests")

    # Wiredtiger options. Prevent fuzzed wt configs from being overwritten unless user specifies it.
    wt_engine_config = config.pop("wt_engine_config")
    if wt_engine_config:
        _config.WT_ENGINE_CONFIG = wt_engine_config
    wt_coll_config = config.pop("wt_coll_config")
    if wt_coll_config:
        _config.WT_COLL_CONFIG = wt_coll_config
    wt_index_config = config.pop("wt_index_config")
    if wt_index_config:
        _config.WT_INDEX_CONFIG = wt_index_config

    # Benchmark/Benchrun options.
    _config.BENCHMARK_FILTER = config.pop("benchmark_filter")
    _config.BENCHMARK_LIST_TESTS = config.pop("benchmark_list_tests")
    benchmark_min_time = config.pop("benchmark_min_time_secs")
    if benchmark_min_time is not None:
        _config.BENCHMARK_MIN_TIME = datetime.timedelta(seconds=benchmark_min_time)
    _config.BENCHMARK_REPETITIONS = config.pop("benchmark_repetitions")

    # Config Dir options.
    _config.CONFIG_DIR = config.pop("config_dir")

    # Directory with jstests option
    _config.JSTESTS_DIR = config.pop("jstests_dir")

    # Configure evergreen task documentation
    if _config.EVERGREEN_TASK_NAME:
        task_name = utils.get_task_name_without_suffix(_config.EVERGREEN_TASK_NAME,
                                                       _config.EVERGREEN_VARIANT_NAME)
        evg_task_doc_file = os.path.join(_config.CONFIG_DIR, "evg_task_doc", "evg_task_doc.yml")
        if os.path.exists(evg_task_doc_file):
            evg_task_doc = utils.load_yaml_file(evg_task_doc_file)
            if task_name in evg_task_doc:
                _config.EVERGREEN_TASK_DOC = evg_task_doc[task_name]

    _config.UNDO_RECORDER_PATH = config.pop("undo_recorder_path")

    _config.EXCLUDE_TAGS_FILE_PATH = config.pop("exclude_tags_file_path")

    _config.MAX_TEST_QUEUE_SIZE = config.pop("max_test_queue_size")

    def configure_tests(test_files, replay_file):
        # `_validate_options` has asserted that at most one of `test_files` and `replay_file` contains input.

        to_replay = None
        # Treat `resmoke run @to_replay` as `resmoke run --replayFile to_replay`
        if len(test_files) == 1 and test_files[0].startswith("@"):
            to_replay = test_files[0][1:]
        elif len(test_files) > 1 and any(test_file.startswith("@") for test_file in test_files):
            parser.error(
                "Cannot use @replay with additional test files listed on the command line invocation."
            )
        elif replay_file:
            to_replay = replay_file

        if to_replay:
            # The replay file is expected to be one file per line, but cope with extra whitespace.
            with open(to_replay) as fd:
                _config.TEST_FILES = fd.read().split()
        else:
            _config.TEST_FILES = test_files

    configure_tests(config.pop("test_files"), config.pop("replay_file"))

    _config.LOGGER_DIR = os.path.join(_config.CONFIG_DIR, "loggers")

    shuffle = config.pop("shuffle")
    if shuffle == "auto":
        # If the user specified a value for --jobs > 1 (or -j > 1), then default to randomize
        # the order in which tests are executed. This is because with multiple threads the tests
        # wouldn't run in a deterministic order anyway.
        _config.SHUFFLE = _config.JOBS > 1
    else:
        _config.SHUFFLE = shuffle == "on"

    conn_string = config.pop("shell_conn_string")
    port = config.pop("shell_port")

    if port is not None:
        conn_string = "mongodb://localhost:" + port

    if conn_string is not None:
        # The --shellConnString command line option must be a MongoDB connection URI, which means it
        # must specify the mongodb:// or mongodb+srv:// URI scheme. pymongo.uri_parser.parse_uri()
        # raises an exception if the connection string specified isn't considered a valid MongoDB
        # connection URI.
        pymongo.uri_parser.parse_uri(conn_string)
        _config.SHELL_CONN_STRING = conn_string

    _config.LOGGER_FILE = config.pop("logger_file")

    if config:
        raise ValueError(f"Unknown option(s): {list(config.keys())}s")


def _set_logging_config():
    """Read YAML configuration from 'pathname' how to log tests and fixtures."""
    pathname = _config.LOGGER_FILE
    try:
        # If the user provides a full valid path to a logging config
        # we don't need to search LOGGER_DIR for the file.
        if os.path.exists(pathname):
            logger_config = utils.load_yaml_file(pathname)
            _config.LOGGING_CONFIG = logger_config.pop("logging")
            _config.SHORTEN_LOGGER_NAME_CONFIG = logger_config.pop("shorten_logger_name")
            return

        root = os.path.abspath(_config.LOGGER_DIR)
        files = os.listdir(root)
        for filename in files:
            (short_name, ext) = os.path.splitext(filename)
            if ext in (".yml", ".yaml") and short_name == pathname:
                config_file = os.path.join(root, filename)
                if not os.path.isfile(config_file):
                    raise ValueError("Expected a logger YAML config, but got '%s'" % pathname)
                logger_config = utils.load_yaml_file(config_file)
                _config.LOGGING_CONFIG = logger_config.pop("logging")
                _config.SHORTEN_LOGGER_NAME_CONFIG = logger_config.pop("shorten_logger_name")
                return

        raise ValueError("Unknown logger '%s'" % pathname)
    except FileNotFoundError:
        raise IOError("Directory {} does not exist.".format(_config.LOGGER_DIR))


def _expand_user(pathname):
    """Provide wrapper around os.path.expanduser() to do nothing when given None."""
    if pathname is None:
        return None
    return os.path.expanduser(pathname)


def _tags_from_list(tags_list):
    """Return the list of tags from a list of tag parameter values.

    Each parameter value in the list may be a list of comma separated tags, with empty strings
    ignored.
    """
    tags = []
    if tags_list is not None:
        for tag in tags_list:
            tags.extend([t for t in tag.split(",") if t != ""])
        return tags
    return None


def _update_symbolizer_secrets():
    """Open `expansions.yml`, get values for symbolizer secrets and update their values inside config.py ."""
    if not _config.EVERGREEN_TASK_ID:
        # not running on Evergreen
        return
    yml_data = utils.load_yaml_file(_config.EXPANSIONS_FILE)
    _config.SYMBOLIZER_CLIENT_SECRET = yml_data.get("symbolizer_client_secret")
    _config.SYMBOLIZER_CLIENT_ID = yml_data.get("symbolizer_client_id")


def add_otel_args(parser: argparse.ArgumentParser):
    parser.add_argument(
        "--otelTraceId",
        dest="otel_trace_id",
        type=str,
        default=os.environ.get("OTEL_TRACE_ID", None),
        help="Open Telemetry Trace ID",
    )

    parser.add_argument(
        "--otelParentId",
        dest="otel_parent_id",
        type=str,
        default=os.environ.get("OTEL_PARENT_ID", None),
        help="Open Telemetry Parent ID",
    )

    parser.add_argument(
        "--otelCollectorDir",
        dest="otel_collector_dir",
        type=str,
        default=os.environ.get("OTEL_COLLECTOR_DIR", "build/metrics/"),
        help="Open Collector Files",
    )


def detect_evergreen_config(parsed_args: argparse.Namespace,
                            expansions_file: str = "../expansions.yml"):
    if not os.path.exists(expansions_file):
        return

    expansions = read_config_file(expansions_file)

    parsed_args.build_id = expansions.get("build_id", None)
    parsed_args.distro_id = expansions.get("distro_id", None)
    parsed_args.execution_number = expansions.get("execution", None)
    parsed_args.project_name = expansions.get("project", None)
    parsed_args.git_revision = expansions.get("revision", None)
    parsed_args.revision_order_id = expansions.get("revision_order_id", None)
    parsed_args.task_id = expansions.get("task_id", None)
    parsed_args.task_name = expansions.get("task_name", None)
    parsed_args.variant_name = expansions.get("build_variant", None)
    parsed_args.version_id = expansions.get("version_id", None)
    parsed_args.work_dir = expansions.get("workdir", None)
    parsed_args.evg_project_config_path = expansions.get("evergreen_config_file_path", None)
