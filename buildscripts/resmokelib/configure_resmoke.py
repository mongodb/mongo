"""Configure the command line input for the resmoke 'run' subcommand."""

import collections
import configparser
import datetime
import os
import os.path
import distutils.spawn
import sys
import platform
import random

import pymongo.uri_parser

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import utils
from buildscripts.resmokelib import mongod_fuzzer_configs


def validate_and_update_config(parser, args):
    """Validate inputs and update config module."""
    _validate_options(parser, args)
    _update_config_vars(args)
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
    error_msgs = {}
    if mongod_set_param_errors:
        error_msgs["mongodSetParameters"] = mongod_set_param_errors
    if mongos_set_param_errors:
        error_msgs["mongosSetParameters"] = mongos_set_param_errors
    if error_msgs:
        parser.error(str(error_msgs))


def _validate_config(parser):  # pylint: disable=too-many-branches
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

        resolved_path = distutils.spawn.find_executable(_config.UNDO_RECORDER_PATH)
        if resolved_path is None:
            parser.error(
                f"Cannot find the UndoDB live-record binary '{_config.UNDO_RECORDER_PATH}'. Check that it exists and is executable"
            )
            return

        if not os.access(resolved_path, os.X_OK):
            parser.error(f"Found '{resolved_path}', but it is not an executable file")


def _update_config_vars(values):  # pylint: disable=too-many-statements,too-many-locals,too-many-branches
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

    if os.path.isfile("resmoke.ini"):
        config_parser = configparser.ConfigParser()
        config_parser.read("resmoke.ini")
        if "resmoke" in config_parser.sections():
            user_config = dict(config_parser["resmoke"])
            config.update(user_config)

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
    _config.FAIL_FAST = not config.pop("continue_on_failure")
    _config.FLOW_CONTROL = config.pop("flow_control")
    _config.FLOW_CONTROL_TICKETS = config.pop("flow_control_tickets")
    _config.INCLUDE_WITH_ANY_TAGS = _tags_from_list(config.pop("include_with_any_tags"))
    _config.GENNY_EXECUTABLE = _expand_user(config.pop("genny_executable"))
    _config.JOBS = config.pop("jobs")
    _config.LINEAR_CHAIN = config.pop("linear_chain") == "on"
    _config.MAJORITY_READ_CONCERN = config.pop("majority_read_concern") == "on"
    _config.MIXED_BIN_VERSIONS = config.pop("mixed_bin_versions")
    if _config.MIXED_BIN_VERSIONS is not None:
        _config.MIXED_BIN_VERSIONS = _config.MIXED_BIN_VERSIONS.split("-")

    _config.INSTALL_DIR = config.pop("install_dir")
    if _config.INSTALL_DIR is not None:
        # Normalize the path so that on Windows dist-test/bin
        # translates to .\dist-test\bin then absolutify it since the
        # Windows PATH variable requires absolute paths.
        _config.INSTALL_DIR = os.path.abspath(_expand_user(os.path.normpath(_config.INSTALL_DIR)))

        for binary in ["mongo", "mongod", "mongos", "dbtest"]:
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
    _config.MONGOD_SET_PARAMETERS = _merge_set_params(config.pop("mongod_set_parameters"))
    _config.FUZZ_MONGOD_CONFIGS = config.pop("fuzz_mongod_configs")
    _config.CONFIG_FUZZ_SEED = config.pop("config_fuzz_seed")

    if _config.FUZZ_MONGOD_CONFIGS:
        if not _config.CONFIG_FUZZ_SEED:
            _config.CONFIG_FUZZ_SEED = random.randrange(sys.maxsize)
        else:
            _config.CONFIG_FUZZ_SEED = int(_config.CONFIG_FUZZ_SEED)
        _config.MONGOD_SET_PARAMETERS, _config.WT_ENGINE_CONFIG = mongod_fuzzer_configs \
            .fuzz_set_parameters(_config.CONFIG_FUZZ_SEED, _config.MONGOD_SET_PARAMETERS)

    _config.MONGOS_EXECUTABLE = _expand_user(config.pop("mongos_executable"))
    _config.MONGOS_SET_PARAMETERS = _merge_set_params(config.pop("mongos_set_parameters"))

    _config.MRLOG = config.pop("mrlog")
    _config.NO_JOURNAL = config.pop("no_journal")
    _config.NUM_CLIENTS_PER_FIXTURE = config.pop("num_clients_per_fixture")
    _config.NUM_REPLSET_NODES = config.pop("num_replset_nodes")
    _config.NUM_SHARDS = config.pop("num_shards")
    _config.PERF_REPORT_FILE = config.pop("perf_report_file")
    _config.RANDOM_SEED = config.pop("seed")
    _config.REPEAT_SUITES = config.pop("repeat_suites")
    _config.REPEAT_TESTS = config.pop("repeat_tests")
    _config.REPEAT_TESTS_MAX = config.pop("repeat_tests_max")
    _config.REPEAT_TESTS_MIN = config.pop("repeat_tests_min")
    _config.REPEAT_TESTS_SECS = config.pop("repeat_tests_secs")
    _config.REPORT_FAILURE_STATUS = config.pop("report_failure_status")
    _config.REPORT_FILE = config.pop("report_file")
    _config.SERVICE_EXECUTOR = config.pop("service_executor")
    _config.SHELL_READ_MODE = config.pop("shell_read_mode")
    _config.SHELL_WRITE_MODE = config.pop("shell_write_mode")
    _config.SPAWN_USING = config.pop("spawn_using")
    _config.STAGGER_JOBS = config.pop("stagger_jobs") == "on"
    _config.STORAGE_ENGINE = config.pop("storage_engine")
    _config.STORAGE_ENGINE_CACHE_SIZE = config.pop("storage_engine_cache_size_gb")
    _config.SUITE_FILES = config.pop("suite_files")
    if _config.SUITE_FILES is not None:
        _config.SUITE_FILES = _config.SUITE_FILES.split(",")
    _config.TAG_FILE = config.pop("tag_file")
    _config.TRANSPORT_LAYER = config.pop("transport_layer")
    _config.USER_FRIENDLY_OUTPUT = config.pop("user_friendly_output")

    # Internal testing options.
    _config.INTERNAL_PARAMS = config.pop("internal_params")

    # Evergreen options.
    _config.EVERGREEN_BUILD_ID = config.pop("build_id")
    _config.EVERGREEN_DISTRO_ID = config.pop("distro_id")
    _config.EVERGREEN_EXECUTION = config.pop("execution_number")
    _config.EVERGREEN_PATCH_BUILD = config.pop("patch_build")
    _config.EVERGREEN_PROJECT_NAME = config.pop("project_name")
    _config.EVERGREEN_REVISION = config.pop("git_revision")
    _config.EVERGREEN_REVISION_ORDER_ID = config.pop("revision_order_id")
    _config.EVERGREEN_TASK_ID = config.pop("task_id")
    _config.EVERGREEN_TASK_NAME = config.pop("task_name")
    _config.EVERGREEN_VARIANT_NAME = config.pop("variant_name")
    _config.EVERGREEN_VERSION_ID = config.pop("version_id")

    # Cedar options.
    _config.CEDAR_URL = config.pop("cedar_url")
    _config.CEDAR_RPC_PORT = config.pop("cedar_rpc_port")

    # Archival options. Archival is enabled only when running on evergreen.
    if not _config.EVERGREEN_TASK_ID:
        _config.ARCHIVE_FILE = None
    _config.ARCHIVE_LIMIT_MB = config.pop("archive_limit_mb")
    _config.ARCHIVE_LIMIT_TESTS = config.pop("archive_limit_tests")

    # Wiredtiger options.
    _config.WT_COLL_CONFIG = config.pop("wt_coll_config")
    wt_engine_config = config.pop("wt_engine_config")
    if wt_engine_config:  # prevents fuzzed wt_engine_config from being overwritten unless user specifies it
        _config.WT_ENGINE_CONFIG = config.pop("wt_engine_config")
    _config.WT_INDEX_CONFIG = config.pop("wt_index_config")

    # Benchmark/Benchrun options.
    _config.BENCHMARK_FILTER = config.pop("benchmark_filter")
    _config.BENCHMARK_LIST_TESTS = config.pop("benchmark_list_tests")
    benchmark_min_time = config.pop("benchmark_min_time_secs")
    if benchmark_min_time is not None:
        _config.BENCHMARK_MIN_TIME = datetime.timedelta(seconds=benchmark_min_time)
    _config.BENCHMARK_REPETITIONS = config.pop("benchmark_repetitions")

    # Config Dir options.
    _config.CONFIG_DIR = config.pop("config_dir")

    _config.UNDO_RECORDER_PATH = config.pop("undo_recorder_path")

    # Populate the named suites by scanning config_dir/suites
    named_suites = {}

    def configure_tests(test_files, replay_file):
        # `_validate_options` has asserted that at most one of `test_files` and `replay_file` contains input.

        to_replay = None
        # Treat `resmoke run @to_replay` as `resmoke run --replayFile to_replay`
        if len(test_files) == 1 and test_files[0].startswith("@"):
            to_replay = test_files[0][1:]
        elif replay_file:
            to_replay = replay_file

        if to_replay:
            # The replay file is expected to be one file per line, but cope with extra whitespace.
            with open(to_replay) as fd:
                _config.TEST_FILES = fd.read().split()
        else:
            _config.TEST_FILES = test_files

    configure_tests(config.pop("test_files"), config.pop("replay_file"))

    suites_dir = os.path.join(_config.CONFIG_DIR, "suites")
    root = os.path.abspath(suites_dir)
    files = os.listdir(root)
    for filename in files:
        (short_name, ext) = os.path.splitext(filename)
        if ext in (".yml", ".yaml"):
            pathname = os.path.join(root, filename)
            named_suites[short_name] = pathname

    _config.NAMED_SUITES = named_suites

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
            _config.LOGGING_CONFIG = utils.load_yaml_file(pathname).pop("logging")
            return

        root = os.path.abspath(_config.LOGGER_DIR)
        files = os.listdir(root)
        for filename in files:
            (short_name, ext) = os.path.splitext(filename)
            if ext in (".yml", ".yaml") and short_name == pathname:
                config_file = os.path.join(root, filename)
                if not os.path.isfile(config_file):
                    raise ValueError("Expected a logger YAML config, but got '%s'" % pathname)
                _config.LOGGING_CONFIG = utils.load_yaml_file(config_file).pop("logging")
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
