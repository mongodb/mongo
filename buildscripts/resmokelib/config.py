"""
Configuration options for resmoke.py.
"""

from __future__ import absolute_import

import collections
import itertools
import os
import os.path
import time


##
# Default values.
##

# Default path for where to look for executables.
DEFAULT_DBTEST_EXECUTABLE = os.path.join(os.curdir, "dbtest")
DEFAULT_MONGO_EXECUTABLE = os.path.join(os.curdir, "mongo")
DEFAULT_MONGOD_EXECUTABLE = os.path.join(os.curdir, "mongod")
DEFAULT_MONGOS_EXECUTABLE = os.path.join(os.curdir, "mongos")

# Default root directory for where resmoke.py puts directories containing data files of mongod's it
# starts, as well as those started by individual tests.
DEFAULT_DBPATH_PREFIX = os.path.normpath("/data/db")

# Subdirectory under the dbpath prefix that contains directories with data files of mongod's started
# by resmoke.py.
FIXTURE_SUBDIR = "resmoke"

# Subdirectory under the dbpath prefix that contains directories with data files of mongod's started
# by individual tests.
MONGO_RUNNER_SUBDIR = "mongorunner"

# Names below correspond to how they are specified via the command line or in the options YAML file.
DEFAULTS = {
    "basePort": 20000,
    "buildloggerUrl": "https://logkeeper.mongodb.org",
    "continueOnFailure": False,
    "dbpathPrefix": None,
    "dbtest": None,
    "distroId": None,
    "dryRun": None,
    "excludeWithAnyTags": None,
    "includeWithAnyTags": None,
    "jobs": 1,
    "mongo": None,
    "mongod": None,
    "mongodSetParameters": None,
    "mongos": None,
    "mongosSetParameters": None,
    "nojournal": False,
    "numClientsPerFixture": 1,
    "shellPort": None,
    "shellConnString": None,
    "patchBuild": False,
    "repeat": 1,
    "reportFailureStatus": "fail",
    "reportFile": None,
    "seed": long(time.time() * 256),  # Taken from random.py code in Python 2.7.
    "serviceExecutor": None,
    "shellReadMode": None,
    "shellWriteMode": None,
    "shuffle": None,
    "staggerJobs": None,
    "storageEngine": None,
    "storageEngineCacheSizeGB": None,
    "tagFile": None,
    "taskId": None,
    "taskName": None,
    "transportLayer": None,
    "variantName": None,
    "wiredTigerCollectionConfigString": None,
    "wiredTigerEngineConfigString": None,
    "wiredTigerIndexConfigString": None
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

# If true, then resmoke.py is being run as part of a patch build in Evergreen.
EVERGREEN_PATCH_BUILD = None

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

##
# Internally used configuration options that aren't exposed to the user
##

# Default sort order for test execution. Will only be changed if --suites wasn't specified.
ORDER_TESTS_BY_NAME = True

# Default file names for externally generated lists of tests created during the build.
DEFAULT_UNIT_TEST_LIST = "build/unittests.txt"
DEFAULT_INTEGRATION_TEST_LIST = "build/integration_tests.txt"

# External files or executables, used as suite selectors, that are created during the build and
# therefore might not be available when creating a test membership map.
EXTERNAL_SUITE_SELECTORS = (DEFAULT_UNIT_TEST_LIST,
                            DEFAULT_INTEGRATION_TEST_LIST,
                            DEFAULT_DBTEST_EXECUTABLE)
