"""
Configuration options for resmoke.py.
"""

from __future__ import absolute_import

import collections
import datetime
import itertools
import os.path
import time


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

# Default path for where to look for executables.
DEFAULT_DBTEST_EXECUTABLE = os.path.join(os.curdir, "dbtest")
DEFAULT_MONGO_EXECUTABLE = os.path.join(os.curdir, "mongo")
DEFAULT_MONGOD_EXECUTABLE = os.path.join(os.curdir, "mongod")
DEFAULT_MONGOS_EXECUTABLE = os.path.join(os.curdir, "mongos")

DEFAULT_BENCHMARK_REPETITIONS = 3
DEFAULT_BENCHMARK_MIN_TIME = datetime.timedelta(seconds=5)

# Default root directory for where resmoke.py puts directories containing data files of mongod's it
# starts, as well as those started by individual tests.
DEFAULT_DBPATH_PREFIX = os.path.normpath("/data/db")

# Names below correspond to how they are specified via the command line or in the options YAML file.
DEFAULTS = {
    "archive_file": None,
    "archive_limit_mb": 5000,
    "archive_limit_tests": 10,
    "base_port": 20000,
    "buildlogger_url": "https://logkeeper.mongodb.org",
    "continue_on_failure": False,
    "dbpath_prefix": None,
    "dbtest_executable": None,
    "dry_run": None,
    "exclude_with_any_tags": None,
    "include_with_any_tags": None,
    "jobs": 1,
    "mongo_executable": None,
    "mongod_executable": None,
    "mongod_set_parameters": None,
    "mongos_executable": None,
    "mongos_set_parameters": None,
    "no_journal": False,
    "num_clients_per_fixture": 1,
    "perf_report_file": None,
    "prealloc_journal": None,  # Default is set on the commandline.
    "repeat": 1,
    "report_failure_status": "fail",
    "report_file": None,
    "seed": long(time.time() * 256),  # Taken from random.py code in Python 2.7.
    "service_executor": None,
    "shell_conn_string": None,
    "shell_port": None,
    "shell_read_mode": None,
    "shell_write_mode": None,
    "shuffle": None,
    "stagger_jobs": None,
    "majority_read_concern": None, # Default is set on the commandline.
    "storage_engine": None,
    "storage_engine_cache_size_gb": None,
    "tag_file": None,
    "transport_layer": None,

    # Evergreen options.
    "distro_id": None,
    "execution_number": 0,
    "git_revision": None,
    "patch_build": False,
    "project_name": "mongodb-mongo-master",
    "task_id": None,
    "task_name": None,
    "variant_name": None,

    # WiredTiger options.
    "wt_coll_config": None,
    "wt_engine_config": None,
    "wt_index_config": None,

    # Benchmark options.
    "benchmark_filter": None,
    "benchmark_list_tests": None,
    "benchmark_min_time_secs": None,
    "benchmark_repetitions": None
}

_SuiteOptions = collections.namedtuple("_SuiteOptions", [
    "description",
    "fail_fast",
    "include_tags",
    "num_jobs",
    "num_repeats",
    "report_failure_status",
])


class SuiteOptions(_SuiteOptions):
    """
    A class for representing top-level options to resmoke.py that can also be set at the
    suite-level.
    """

    INHERIT = object()
    ALL_INHERITED = None

    @classmethod
    def combine(cls, *suite_options_list):
        """
        Returns a SuiteOptions instance representing the combination of all SuiteOptions in
        'suite_options_list'.
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
        """
        Returns a SuiteOptions instance representing the options overridden at the suite-level and
        the inherited options from the top-level.
        """

        description = None
        include_tags = None
        parent = dict(zip(SuiteOptions._fields, [
            description,
            FAIL_FAST,
            include_tags,
            JOBS,
            REPEAT,
            REPORT_FAILURE_STATUS,
        ]))

        options = self._asdict()
        for field in SuiteOptions._fields:
            if options[field] is SuiteOptions.INHERIT:
                options[field] = parent[field]

        return SuiteOptions(**options)


SuiteOptions.ALL_INHERITED = SuiteOptions(**dict(zip(SuiteOptions._fields,
                                                     itertools.repeat(SuiteOptions.INHERIT))))

##
# Variables that are set by the user at the command line or with --options.
##

# The name of the archive JSON file used to associate S3 archives to an Evergreen task.
ARCHIVE_FILE = None

# The limit size of all archive files for an Evergreen task.
ARCHIVE_LIMIT_MB = None

# The limit number of tests to archive for an Evergreen task.
ARCHIVE_LIMIT_TESTS = None

# The starting port number to use for mongod and mongos processes spawned by resmoke.py and the
# mongo shell.
BASE_PORT = None

# The root url of the buildlogger server.
BUILDLOGGER_URL = None

# Root directory for where resmoke.py puts directories containing data files of mongod's it starts,
# as well as those started by individual tests.
DBPATH_PREFIX = None

# The path to the dbtest executable used by resmoke.py.
DBTEST_EXECUTABLE = None

# If set to "tests", then resmoke.py will output the tests that would be run by each suite (without
# actually running them).
DRY_RUN = None

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

# The identifier for the Evergreen task that resmoke.py is being run under. If set, then the
# Evergreen task id value will be transmitted to logkeeper when creating builds and tests.
EVERGREEN_TASK_ID = None

# The name of the Evergreen task that resmoke.py is being run for.
EVERGREEN_TASK_NAME = None

# The name of the Evergreen build variant that resmoke.py is being run on.
EVERGREEN_VARIANT_NAME = None

# If set, then any jstests that have any of the specified tags will be excluded from the suite(s).
EXCLUDE_WITH_ANY_TAGS = None

# If true, then a test failure or error will cause resmoke.py to exit and not run any more tests.
FAIL_FAST = None

# If set, then only jstests that have at least one of the specified tags will be run during the
# jstest portion of the suite(s).
INCLUDE_WITH_ANY_TAGS = None

# If set, then resmoke.py starts the specified number of Job instances to run tests.
JOBS = None

# The path to the mongo executable used by resmoke.py.
MONGO_EXECUTABLE = None

# The path to the mongod executable used by resmoke.py.
MONGOD_EXECUTABLE = None

# The --setParameter options passed to mongod.
MONGOD_SET_PARAMETERS = None

# The path to the mongos executable used by resmoke.py.
MONGOS_EXECUTABLE = None

# The --setParameter options passed to mongos.
MONGOS_SET_PARAMETERS = None

# If true, then all mongod's started by resmoke.py and by the mongo shell will not have journaling
# enabled.
NO_JOURNAL = None

# If true, then all mongod's started by resmoke.py and by the mongo shell will not preallocate
# journal files.
NO_PREALLOC_JOURNAL = None

# If set, then each fixture runs tests with the specified number of clients.
NUM_CLIENTS_PER_FIXTURE = None

# Report file for the Evergreen performance plugin.
PERF_REPORT_FILE = None

# If set, then the RNG is seeded with the specified value. Otherwise uses a seed based on the time
# this module was loaded.
RANDOM_SEED = None

# If set, then each suite is repeated the specified number of times.
REPEAT = None

# Controls if the test failure status should be reported as failed or be silently ignored.
REPORT_FAILURE_STATUS = None

# If set, then resmoke.py will write out a report file with the status of each test that ran.
REPORT_FILE = None

# IF set, then mongod/mongos's started by resmoke.py will use the specified service executor
SERVICE_EXECUTOR = None

# If set, resmoke will override the default fixture and connect to the fixture specified by this
# connection string instead.
SHELL_CONN_STRING = None

# If set, then mongo shells started by resmoke.py will use the specified read mode.
SHELL_READ_MODE = None

# If set, then mongo shells started by resmoke.py will use the specified write mode.
SHELL_WRITE_MODE = None

# If true, then the order the tests run in is randomized. Otherwise the tests will run in
# alphabetical (case-insensitive) order.
SHUFFLE = None

# If true, the launching of jobs is staggered in resmoke.py.
STAGGER_JOBS = None

# If set to true, it enables read concern majority. Else, read concern majority is disabled.
MAJORITY_READ_CONCERN = None;

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# storage engine.
STORAGE_ENGINE = None

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# storage engine cache size.
STORAGE_ENGINE_CACHE_SIZE = None

# The tag file to use that associates tests with tags.
TAG_FILE = None

# If set, then mongod/mongos's started by resmoke.py will use the specified transport layer.
TRANSPORT_LAYER = None

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

##
# Internally used configuration options that aren't exposed to the user
##

# S3 Bucket to upload archive files.
ARCHIVE_BUCKET = "mongodatafiles"

# Benchmark options set internally by resmoke.py
BENCHMARK_OUT_FORMAT = "json"

# Default sort order for test execution. Will only be changed if --suites wasn't specified.
ORDER_TESTS_BY_NAME = True

# Default file names for externally generated lists of tests created during the build.
DEFAULT_BENCHMARK_TEST_LIST = "build/benchmarks.txt"
DEFAULT_UNIT_TEST_LIST = "build/unittests.txt"
DEFAULT_INTEGRATION_TEST_LIST = "build/integration_tests.txt"

# External files or executables, used as suite selectors, that are created during the build and
# therefore might not be available when creating a test membership map.
EXTERNAL_SUITE_SELECTORS = (DEFAULT_BENCHMARK_TEST_LIST,
                            DEFAULT_UNIT_TEST_LIST,
                            DEFAULT_INTEGRATION_TEST_LIST,
                            DEFAULT_DBTEST_EXECUTABLE)
