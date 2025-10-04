"""Configuration options for resmoke.py."""

import collections
import datetime
import itertools
import os.path
import time

import buildscripts.resmokelib.setup_multiversion.config as multiversion_config

# Subdirectory under the dbpath prefix that contains directories with data files of mongod's started
# by resmoke.py.
FIXTURE_SUBDIR = "resmoke"

# Subdirectory under the dbpath prefix that contains directories with data files of mongod's started
# by individual tests.
MONGO_RUNNER_SUBDIR = "mongorunner"

##
# Default values. There are two types of default values: "DEFAULT_" prefixed module variables,
# and values in the "DEFAULTS" dictionary. The former is used to set the default value manually.
# (e.g. if the default value needs to be reconciled with suite-level configuration)
# The latter is set automatically as part of resmoke's option parsing on startup.
##

# We default to search for executables in the current working directory or in /data/multiversion
# which are both part of the PATH.
DEFAULT_DBTEST_EXECUTABLE = os.path.join(os.curdir, "dbtest")
DEFAULT_MONGO_EXECUTABLE = "mongo"
DEFAULT_MONGOD_EXECUTABLE = "mongod"
DEFAULT_MONGOS_EXECUTABLE = "mongos"
DEFAULT_MONGOT_EXECUTABLE = "mongot-localdev/mongot"
DEFAULT_MONGOTEST_EXECUTABLE = "mongotest"

DEFAULT_BENCHMARK_REPETITIONS = 3
DEFAULT_BENCHMARK_MIN_TIME = datetime.timedelta(seconds=5)

# Default root directory for where resmoke.py puts directories containing data files of mongod's it
# starts, as well as those started by individual tests.
DEFAULT_DBPATH_PREFIX = os.path.normpath("/data/db")

# Default directory that we expect to contain binaries for multiversion testing. This directory is
# added to the PATH when calling programs.make_process().
DEFAULT_MULTIVERSION_DIRS = [os.path.normpath("/data/multiversion")]
if os.path.isfile(multiversion_config.WINDOWS_BIN_PATHS_FILE):
    with open(multiversion_config.WINDOWS_BIN_PATHS_FILE) as wbpf:
        DEFAULT_MULTIVERSION_DIRS.extend(wbpf.read().split(os.pathsep))

# Default location for the genny executable. Override this in the YAML suite configuration if
# desired.
DEFAULT_GENNY_EXECUTABLE = os.path.normpath("genny/build/src/driver/genny")

# Names below correspond to how they are specified via the command line or in the options YAML file.
DEFAULTS = {
    "auto_kill": "on",
    "always_use_log_files": False,
    "archive_limit_mb": 5000,
    "archive_limit_tests": 10,
    "archive_directory": None,
    "archive_mode": "s3",
    "base_port": 20000,
    "backup_on_restart_dir": None,
    "config_shard": None,
    "continue_on_failure": False,
    "test_timeout": None,
    "dbpath_prefix": None,
    "dbtest_executable": None,
    "dry_run": None,
    "exclude_with_any_tags": None,
    "force_excluded_tests": False,
    "skip_excluded_tests": False,
    "skip_tests_covered_by_more_complex_suites": False,
    "include_fully_disabled_feature_tests": False,
    "skip_symbolization": False,
    "fuzz_mongod_configs": None,
    "fuzz_runtime_params": None,
    "fuzz_runtime_stress": "off",
    "fuzz_mongos_configs": None,
    "config_fuzz_seed": None,
    "disable_encryption_fuzzing": False,
    "genny_executable": None,
    "include_with_any_tags": None,
    "include_with_all_tags": None,
    "install_dir": None,
    "jobs": 1,
    "logger_file": "console",
    "mongo_executable": None,
    "mongod_executable": None,
    "mongod_set_parameters": [],
    "mongos_executable": None,
    "mongos_set_parameters": [],
    "mongot-localdev/mongot_executable": None,
    "mongot_set_parameters": [],
    "mongocryptd_set_parameters": [],
    "mongo_set_parameters": [],
    "mrlog": None,
    "multiversion_dirs": DEFAULT_MULTIVERSION_DIRS,
    "no_journal": False,
    "num_clients_per_fixture": 1,
    "use_tenant_client": False,
    "origin_suite": None,
    "cedar_report_file": None,
    "repeat_suites": 1,
    "repeat_tests": 1,
    "repeat_tests_max": None,
    "repeat_tests_min": None,
    "repeat_tests_secs": None,
    "replay_file": None,
    "report_file": None,
    "run_all_feature_flag_tests": False,
    "run_no_feature_flag_tests": False,
    "additional_feature_flags": None,
    "additional_feature_flags_file": None,
    "excluded_feature_flags": None,
    "disable_unreleased_ifr_flags": False,
    "seed": int(time.time() * 256),  # Taken from random.py code in Python 2.7.
    "service_executor": None,
    "shard_count": None,
    "shard_index": None,
    "shell_conn_string": None,
    "historic_test_runtimes": None,
    "shell_port": None,
    "shuffle": None,
    "stagger_jobs": None,
    "majority_read_concern": "on",
    "modules": "default",
    "resmoke_modules_path": os.path.join("buildscripts", "resmokeconfig", "resmoke_modules.yml"),
    "enable_evergreen_api_test_selection": False,
    "test_selection_strategies_array": None,
    "mongo_version_file": None,
    "releases_file": None,
    "shell_seed": None,
    "storage_engine": "wiredTiger",
    "storage_engine_cache_size_gb": None,
    "storage_engine_cache_size_pct": None,
    "mozjs_js_gc_zeal": None,
    "suite_files": "with_server",
    "tag_files": [],
    "test_files": [],
    "user_friendly_output": None,
    # Output log format.
    "log_format": None,
    # Level of log severity.
    "log_level": None,
    "mixed_bin_versions": None,
    "old_bin_version": None,
    "linear_chain": None,
    "num_replset_nodes": None,
    "num_shards": None,
    "export_mongod_config": "off",
    "tls_mode": None,
    "tls_ca_file": None,
    "shell_grpc": False,
    "shell_tls_enabled": False,
    "shell_tls_certificate_key_file": None,
    "mongos_tls_certificate_key_file": None,
    "mongod_tls_certificate_key_file": None,
    "validate_selector_paths": True,
    # Internal testing options.
    "internal_params": [],
    # Evergreen options.
    "evergreen_url": "evergreen.mongodb.com",
    "build_id": None,
    "distro_id": None,
    "execution_number": 0,
    "git_revision": None,
    "patch_build": False,
    "project_name": "mongodb-mongo-master",
    "revision_order_id": None,
    "task_id": None,
    "task_name": None,
    "task_doc": None,
    "variant_name": None,
    "version_id": None,
    "work_dir": None,
    "evg_project_config_path": "etc/evergreen.yml",
    "requester": None,
    # WiredTiger options.
    "wt_coll_config": None,
    "wt_engine_config": None,
    "wt_index_config": None,
    # Benchmark options.
    "benchmark_filter": None,
    "benchmark_list_tests": None,
    "benchmark_min_time_secs": None,
    "benchmark_repetitions": None,
    # Config Dir
    "config_dir": "buildscripts/resmokeconfig",
    # Directory with jstests
    "jstests_dir": "jstests",
    # Generate multiversion exclude tags options
    "exclude_tags_file_path": "generated_resmoke_config/multiversion_exclude_tags.yml",
    # Limit the number of tests to execute
    "max_test_queue_size": None,
    # Sanity check
    "sanity_check": False,
    # otel info
    "otel_trace_id": None,
    "otel_parent_id": None,
    "otel_collector_dir": None,
    # The images to build for an External System Under Test
    "docker_compose_build_images": None,
    # Where the `--dockerComposeBuildImages` is happening.
    "docker_compose_build_env": "local",
    # Tag to use for images built & used for an External System Under Test
    "docker_compose_tag": "development",
    # Whether or not this resmoke suite is running against an External System Under Test
    "external_sut": False,
    # Whether or not to signal tests to pause after dataset population.
    "pause_after_populate": None,
    # Regex to filter mocha-style tests to run.
    "mocha_grep": None,
    # Loads all test extensions into the server.
    "load_all_extensions": False,
}

_SuiteOptions = collections.namedtuple(
    "_SuiteOptions",
    [
        "description",
        "fail_fast",
        "include_tags",
        "num_jobs",
        "num_repeat_suites",
        "num_repeat_tests",
        "num_repeat_tests_max",
        "num_repeat_tests_min",
        "time_repeat_tests_secs",
    ],
)


class SuiteOptions(_SuiteOptions):
    """Represent top-level options to resmoke.py that can also be set at the suite-level."""

    INHERIT = object()
    ALL_INHERITED = None

    @classmethod
    def combine(cls, *suite_options_list):
        """Return SuiteOptions instance.

        This object represents the combination of all SuiteOptions in 'suite_options_list'.
        """

        combined_options = cls.ALL_INHERITED._asdict()
        include_tags_list = []

        for suite_options in suite_options_list:
            for field in cls._fields:
                value = getattr(suite_options, field)
                if value is cls.INHERIT:
                    continue

                if field == "description":
                    # We discard the description of each of the individual SuiteOptions when they
                    # are combined.
                    continue

                if field == "include_tags":
                    if value is not None:
                        include_tags_list.append(value)
                    continue

                combined_value = combined_options[field]
                if combined_value is not cls.INHERIT and combined_value != value:
                    raise ValueError("Attempted to set '{}' option multiple times".format(field))
                combined_options[field] = value

        if include_tags_list:
            combined_options["include_tags"] = {"$allOf": include_tags_list}

        return cls(**combined_options)

    def resolve(self):
        """Return a SuiteOptions instance.

        This represents the options overridden at the suite-level and
        the inherited options from the top-level.
        """

        description = None
        include_tags = None
        parent = dict(
            list(
                zip(
                    SuiteOptions._fields,
                    [
                        description,
                        FAIL_FAST,
                        include_tags,
                        JOBS,
                        REPEAT_SUITES,
                        REPEAT_TESTS,
                        REPEAT_TESTS_MAX,
                        REPEAT_TESTS_MIN,
                        REPEAT_TESTS_SECS,
                    ],
                )
            )
        )

        options = self._asdict()
        for field in SuiteOptions._fields:
            if options[field] is SuiteOptions.INHERIT:
                options[field] = parent[field]

        return SuiteOptions(**options)


SuiteOptions.ALL_INHERITED = SuiteOptions(  # type: ignore
    **dict(list(zip(SuiteOptions._fields, itertools.repeat(SuiteOptions.INHERIT))))
)


class MultiversionOptions(object):
    """Represent the multiversion version choices."""

    LAST_LTS = "last_lts"
    LAST_CONTINUOUS = "last_continuous"

    @classmethod
    def all_options(cls):
        """Return available version options for multiversion."""

        return [cls.LAST_LTS, cls.LAST_CONTINUOUS]


##
# Variables that are set by the user at the command line or with --options.
##

# Allow resmoke permission to automatically kill existing rogue mongo processes.
AUTO_KILL = "on"

# Log to files located in the db path and don't clean dbpaths after tests.
ALWAYS_USE_LOG_FILES = False

# The limit size of all archive files for an Evergreen task.
ARCHIVE_LIMIT_MB = None

# The limit number of tests to archive for an Evergreen task.
ARCHIVE_LIMIT_TESTS = None

# Mechanism to use for storing archived data files on failures.
ARCHIVE_MODE = None

# Whether to back up data when restarting a process.
BACKUP_ON_RESTART_DIR = None

# The starting port number to use for mongod and mongos processes spawned by resmoke.py and the
# mongo shell.
BASE_PORT = None

# Root directory for where resmoke.py puts directories containing data files of mongod's it starts,
# as well as those started by individual tests.
DBPATH_PREFIX = None

# The path to the dbtest executable used by resmoke.py.
DBTEST_EXECUTABLE = None

# Directories to search for multiversion binaries
MULTIVERSION_DIRS = []

# If set to "tests", then resmoke.py will output the tests that would be run by each suite (without
# actually running them).
DRY_RUN = None

# If set, specifies which node is the config shard. Can also be set to 'any'.
CONFIG_SHARD = None

# path for the resmoke module config, should only be changed in testing.
MODULES_CONFIG_PATH = None

# list of enabled modules
MODULES = None

# list of dirs from enabled modules to get suites from
MODULE_SUITE_DIRS = []

# list of dirs from disabled modules to filter out of suite selectors
MODULE_DISABLED_JSTEST_DIRS = []

# if set, enables test selection using the Evergreen API
ENABLE_EVERGREEN_API_TEST_SELECTION = None

# If set, requests Evergreen to use the specified test selection strategies.
EVERGREEN_TEST_SELECTION_STRATEGY = None

# Path to the YAML file containing the current `mongo_version`
MONGO_VERSION_FILE = ".resmoke_mongo_version.yml"

# Path to the releases YAML file.
RELEASES_FILE = ".resmoke_mongo_release_values.yml"

# URL to connect to the Evergreen service.
EVERGREEN_URL = None

# An identifier consisting of the project name, build variant name, commit hash, and the timestamp.
# For patch builds, it also includes the patch version id.
EVERGREEN_BUILD_ID = None

# The identifier for the Evergreen distro that resmoke.py is being run on.
EVERGREEN_DISTRO_ID = None

# The number of the Evergreen execution that resmoke.py is being run on.
EVERGREEN_EXECUTION = None

# If true, then resmoke.py is being run as part of a patch build in Evergreen.
EVERGREEN_PATCH_BUILD = None

# The name of the Evergreen project that resmoke.py is being run on.
EVERGREEN_PROJECT_NAME = None

# The git revision of the Evergreen task that resmoke.py is being run on.
EVERGREEN_REVISION = None

# A number for the chronological order of this revision.
EVERGREEN_REVISION_ORDER_ID = None

# The identifier for the Evergreen task that resmoke.py is being run under. If set, then the
# Evergreen task id value will be transmitted to logkeeper when creating builds and tests.
EVERGREEN_TASK_ID = None

# The name of the Evergreen task that resmoke.py is being run for.
EVERGREEN_TASK_NAME = None

# The documentation that describes what Evergreen task does.
EVERGREEN_TASK_DOC = None

# The name of the Evergreen build variant that resmoke.py is being run on.
EVERGREEN_VARIANT_NAME = None

# The identifier consisting of the project name and the commit hash. For patch builds, it is just
# the commit hash.
EVERGREEN_VERSION_ID = None

# The Evergreen task's working directory.
EVERGREEN_WORK_DIR = None

# Path to evergreen project configuration yaml file
EVERGREEN_PROJECT_CONFIG_PATH = None

# What triggered the task: patch, github_pr, github_tag, commit, trigger, commit_queue, or ad_hoc
EVERGREEN_REQUESTER = None

# If set, then any jstests that have any of the specified tags will be excluded from the suite(s).
EXCLUDE_WITH_ANY_TAGS = None

# Allow test files passed as positional args to run even if they are excluded on the suite config.
FORCE_EXCLUDED_TESTS = None

# Allow a suite to continue its execution if the list of test files passed as positional args
# contains any excluded test. This flag gets ignored when FORCE_EXCLUDED_TESTS is also set.
SKIP_EXCLUDED_TESTS = None

# Only run tests on the given suite that will not be run on a more complex suite.
SKIP_TESTS_COVERED_BY_MORE_COMPLEX_SUITES = None

# Include tests tagged with features that are in fully_disabled_feature_flags.yml. Used for
# test discovery during task generation.
INCLUDE_FULLY_DISABLED_FEATURE_TESTS = None

# Skip symbolizing stacktraces generated during tests.
SKIP_SYMBOLIZATION = None

# A tag which is implicited excluded. This is useful for temporarily disabling a test.
EXCLUDED_TAG = "__TEMPORARILY_DISABLED__"

# If true, then a test failure or error will cause resmoke.py to exit and not run any more tests.
FAIL_FAST = None

# Timeout for execution of a single test, in seconds.
TEST_TIMEOUT = None

# Defines how to fuzz mongod parameters on startup
FUZZ_MONGOD_CONFIGS = None

# Defines how to fuzz mongod/mongos parameters at test run-time
FUZZ_RUNTIME_PARAMS = None

# Defines how to stress the system at run-time
FUZZ_RUNTIME_STRESS = None

# Defines how to fuzz mongos parameters on startup
FUZZ_MONGOS_CONFIGS = None

# This seeds the random number generator used to fuzz mongod and mongos parameters
CONFIG_FUZZ_SEED = None

# Disables the fuzzing that sometimes enables the encrypted storage engine.
DISABLE_ENCRYPTION_FUZZING = None

# Executable file for genny, passed in as a command line arg.
GENNY_EXECUTABLE = None

# If set, then only jstests that have at least one of the specified tags will be run during the
# jstest portion of the suite(s).
INCLUDE_WITH_ANY_TAGS = None

# If set, only jstests that have all the tags will be run.
INCLUDE_TAGS = None

# Params that can be set to change internal resmoke behavior. Used to test resmoke and should
# not be set by the user.
INTERNAL_PARAMS = []

# If set, then resmoke.py starts the specified number of Job instances to run tests.
JOBS = None

# Yaml file that specified logging configuration.
LOGGER_FILE = None

# Where to find the MONGO*_EXECUTABLE binaries
INSTALL_DIR = None

# Whether to run tests for feature flags.
RUN_ALL_FEATURE_FLAG_TESTS = None

# Whether to run the tests with enabled feature flags
RUN_NO_FEATURE_FLAG_TESTS = None

# the path to a file containing feature flags
ADDITIONAL_FEATURE_FLAGS_FILE = None

# List of feature flags to disable
DISABLED_FEATURE_FLAGS = None

# List of feature flags to enable.
ENABLED_FEATURE_FLAGS = []

# The path to the mongo executable used by resmoke.py.
MONGO_EXECUTABLE = None

# The path to the mongod executable used by resmoke.py.
MONGOD_EXECUTABLE = None

# The --setParameter options passed to mongod.
MONGOD_SET_PARAMETERS = []

# Extra configurations for mongod (i.e. ones that aren't setParameters). Unlike other configuration
# options, these "extra" configs are only used to invoke resmoke's standalone mongod instance. Extra
# configs are a dictionary of {flag: bool, ...} which will become command-line args to mongod of the
# form "--flag" if true, and nothing if false.
MONGOD_EXTRA_CONFIG = {}

# The path to the mongos executable used by resmoke.py.
MONGOS_EXECUTABLE = None

# The --setParameter options passed to mongos.
MONGOS_SET_PARAMETERS = []

# The path to the mongot executable used by resmoke.py.
MONGOT_EXECUTABLE = None

# The --setParameter options passed to mongot.
MONGOT_SET_PARAMETERS = []

# The --setParameter options passed to mongocryptd.
MONGOCRYPTD_SET_PARAMETERS = []

# The --setParameter options passed to mongo shell.
MONGO_SET_PARAMETERS = []

# If true, then all mongod's started by resmoke.py and by the mongo shell will not have journaling
# enabled.
NO_JOURNAL = None

# If set, then each fixture runs tests with the specified number of clients.
NUM_CLIENTS_PER_FIXTURE = None

# If set, each client will be constructed with a generated tenant id.
USE_TENANT_CLIENT = False

# Indicates the name of the test suite prior to the suite being split up by suite generation
ORIGIN_SUITE = None

# Report file for Cedar.
CEDAR_REPORT_FILE = None

# If set, then the RNG is seeded with the specified value. Otherwise uses a seed based on the time
# this module was loaded.
RANDOM_SEED = None

# If set, then each suite is repeated the specified number of times.
REPEAT_SUITES = None

# If set, then each test is repeated the specified number of times inside the suites.
REPEAT_TESTS = None

# If set and REPEAT_TESTS_SECS is set, then each test is repeated up to specified number of
# times inside the suites.
REPEAT_TESTS_MAX = None

# If set and REPEAT_TESTS_SECS is set, then each test is repeated at least specified number of
# times inside the suites.
REPEAT_TESTS_MIN = None

# If set, then each test is repeated the specified time (seconds) inside the suites.
REPEAT_TESTS_SECS = None

# If set, then resmoke.py will write out a report file with the status of each test that ran.
REPORT_FILE = None

# IF set, then mongod/mongos's started by resmoke.py will use the specified service executor
SERVICE_EXECUTOR = None

# If set, resmoke will override the default fixture and connect to the fixture specified by this
# connection string instead.
SHELL_CONN_STRING = None

# If set, resmoke will override the random seed for jstests.
SHELL_SEED = None

# If true, then the order the tests run in is randomized. Otherwise the tests will run in
# alphabetical (case-insensitive) order.
SHUFFLE_STRATEGY = None

# If true, the launching of jobs is staggered in resmoke.py.
STAGGER_JOBS = None

# If set to true, it enables read concern majority. Else, read concern majority is disabled.
MAJORITY_READ_CONCERN = None

# Specifies the binary versions of each node we should run for a replica set.
MIXED_BIN_VERSIONS = None

# Specifies the binary version of last-lts or last-continous when multiversion enabled
MULTIVERSION_BIN_VERSION = None

# Specifies whether to use gRPC when connecting via the shell by default.
SHELL_GRPC = None

# Specifies what tlsMode the server(s) should be started with.
TLS_MODE = None

# Specifies the CA file that all servers and shell instances should use.
TLS_CA_FILE = None

# Shell TLS options.
SHELL_TLS_ENABLED = None
SHELL_TLS_CERTIFICATE_KEY_FILE = None

# Specifies the TLS certicicate that all mongods in the cluster should use.
MONGOD_TLS_CERTIFICATE_KEY_FILE = None

# Specifies the TLS certicicate that all mongoses in the cluster should use.
MONGOS_TLS_CERTIFICATE_KEY_FILE = None

# Specifies the number of replica set members in a ReplicaSetFixture.
NUM_REPLSET_NODES = None

# Specifies the number of replica sets in a MultiReplicaSetFixture.
NUM_REPLSETS = None

# Specifies the number of shards in a ShardedClusterFixture.
NUM_SHARDS = None

# Specifies whether to export the history of mongod config options.
EXPORT_MONGOD_CONFIG = None

# If true, run ReplicaSetFixture with linear chaining.
LINEAR_CHAIN = None

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# storage engine.
STORAGE_ENGINE = None

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# storage engine cache size.
STORAGE_ENGINE_CACHE_SIZE = None

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# storage engine cache size.
STORAGE_ENGINE_CACHE_SIZE_PCT = None

# Yaml suites that specify how tests should be executed.
SUITE_FILES = None

# The tag file to use that associates tests with tags.
TAG_FILES = None

# The test files to execute.
TEST_FILES = None

# Metrics for open telemetry
OTEL_TRACE_ID = None
OTEL_PARENT_ID = None
OTEL_COLLECTOR_DIR = None

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# WiredTiger collection configuration settings.
WT_COLL_CONFIG = None

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# WiredTiger storage engine configuration settings.
WT_ENGINE_CONFIG = None

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# WiredTiger index configuration settings.
WT_INDEX_CONFIG = None

# Benchmark options that map to Google Benchmark options when converted to lowercase.
BENCHMARK_FILTER = None
BENCHMARK_LIST_TESTS = None
BENCHMARK_MIN_TIME = None
BENCHMARK_REPETITIONS = None

# # Generate multiversion exclude tags options
EXCLUDE_TAGS_FILE_PATH = None

# Limit the number of tests to execute
MAX_TEST_QUEUE_SIZE = None

# The total shard count.
SHARD_COUNT = None

# The shard index of the shard to run.
SHARD_INDEX = None

# JSON containing historic test runtimes
HISTORIC_TEST_RUNTIMES = None

##
# Internally used configuration options that aren't exposed to the user
##

# The name of the archive JSON file used to associate S3 archives to an Evergreen task.
ARCHIVE_FILE = "archive.json"

# S3 Bucket to upload archive files.
ARCHIVE_BUCKET = "mongodatafiles"

# Directory to store archive files.
ARCHIVE_DIRECTORY = None

# Force archive all files where appropriate. Eventually we want this to be the default option.
# For now, only the mainline required builders have this option enabled.
FORCE_ARCHIVE_ALL_DATA_FILES = False

# Benchmark options set internally by resmoke.py
BENCHMARK_OUT_FORMAT = "json"

# Default sort order for test execution. Will only be changed if --suites wasn't specified.
ORDER_TESTS_BY_NAME = True

# Default file names for externally generated lists of tests created during the build.
DEFAULT_BENCHMARK_TEST_LIST = "bazel-bin/install/install-mongo_benchmark-stripped_test_list.txt"
DEFAULT_UNIT_TEST_LIST = "bazel-bin/install/install-mongo_unittest_test_list.txt"
DEFAULT_INTEGRATION_TEST_LIST = "bazel-bin/install/install-mongo_integration_test_test_list.txt"
DEFAULT_LIBFUZZER_TEST_LIST = "bazel-bin/install/install-mongo_fuzzer_test_test_list.txt"
DEFAULT_PRETTY_PRINTER_TEST_LIST = "bazel-bin/install/install-dist-test-stripped_test_list.txt"
SPLIT_UNITTESTS_LISTS = [
    f"bazel-bin/install/install-mongo_unittest_{test_group}_group_test_list.txt"
    for test_group in ["first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth"]
]
BENCHMARK_SUITE_TEST_LISTS = [
    "bazel-bin/install/install-repl_bm_test_list.txt",
    "bazel-bin/install/install-query_bm_test_list.txt",
    "bazel-bin/install/install-bsoncolumn_bm_test_list.txt",
    "bazel-bin/install/install-first_half_bm_test_list.txt",
    "bazel-bin/install/install-second_half_bm_test_list.txt",
    "bazel-bin/install/install-storage_bm_test_list.txt",
    "bazel-bin/install/install-sharding_bm_test_list.txt",
    "bazel-bin/install/install-sep_bm_test_list.txt",
]
# External files or executables, used as suite selectors, that are created during the build and
# therefore might not be available when creating a test membership map.
EXTERNAL_SUITE_SELECTORS = (
    DEFAULT_BENCHMARK_TEST_LIST,
    DEFAULT_UNIT_TEST_LIST,
    DEFAULT_INTEGRATION_TEST_LIST,
    DEFAULT_DBTEST_EXECUTABLE,
    DEFAULT_LIBFUZZER_TEST_LIST,
    DEFAULT_PRETTY_PRINTER_TEST_LIST,
    *SPLIT_UNITTESTS_LISTS,
    *BENCHMARK_SUITE_TEST_LISTS,
)

# Where to look for logging and suite configuration files
CONFIG_DIR = None
LOGGER_DIR = None

# Where to look for jstests existence
JSTESTS_DIR = None

# Generated logging config for the current invocation.
LOGGING_CONFIG: dict = {}
SHORTEN_LOGGER_NAME_CONFIG: dict = {}

# Whether legacy multiversion code is used. This value is not used on the master version of
# resmoke.py but is needed to ensure the v5.0 version of fixture classes (e.g. standalone.py)
# that get loaded for multiversion tests can behave correctly on master and on v5.0; the latter
# case runs 5.0 and 4.4 binaries and has this value set to True. Can be removed after 6.0.
USE_LEGACY_MULTIVERSION = True

# Expansions file location
# in CI, the expansions file is located in the ${workdir}, one dir up
# from src, the checkout directory
EXPANSIONS_FILE = "../expansions.yml" if "CI" in os.environ else "expansions.yml"

# Symbolizer secrets
SYMBOLIZER_CLIENT_SECRET = None
SYMBOLIZER_CLIENT_ID = None

# Sanity check
SANITY_CHECK = False

# The images to build for an External System Under Test
DOCKER_COMPOSE_BUILD_IMAGES = None

# Where the `--dockerComposeBuildImages` is happening.
DOCKER_COMPOSE_BUILD_ENV = "local"

# Tag to use for images built & used for an External System Under Test
DOCKER_COMPOSE_TAG = "development"

# Whether or not this resmoke suite is running against an External System Under Test
EXTERNAL_SUT = False

# This will be set to True when:
# (1) We are building images for an External SUT
# (2) Running against an External SUT
NOOP_MONGO_D_S_PROCESSES = False

# If resmoke is running from within a `workload` container,
# we may need to do additional setup to run the suite successfully.
REQUIRES_WORKLOAD_CONTAINER_SETUP = False

# Config fuzzer encryption options, this is only set when the fuzzer is run
CONFIG_FUZZER_ENCRYPTION_OPTS = None

# If resmoke is running on a build variant that specifies a mongo_mozjs_opts,
# we need a way to provide the JS_GC_ZEAL setting provided as part of the mongo_mozjs_opts
# exclusively to mongod/mongos.
MOZJS_JS_GC_ZEAL = None

# If resmoke should check that all paths in suite config selectors are valid.
VALIDATE_SELECTOR_PATHS = True

# If set, resmoke.py will set Testdata.pauseAfterPopulate to allow tests that check this
# flag to pause after populating their initial datasets.
PAUSE_AFTER_POPULATE = None

# Loads all test extensions into the server.
LOAD_ALL_EXTENSIONS = False
