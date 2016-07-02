"""
Configuration options for resmoke.py.
"""

from __future__ import absolute_import

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
    "dryRun": None,
    "excludeWithAllTags": None,
    "excludeWithAnyTags": None,
    "includeWithAllTags": None,
    "includeWithAnyTags": None,
    "jobs": 1,
    "mongo": None,
    "mongod": None,
    "mongodSetParameters": None,
    "mongos": None,
    "mongosSetParameters": None,
    "nojournal": False,
    "numClientsPerFixture": 1,
    "repeat": 1,
    "reportFile": None,
    "seed": long(time.time() * 256),  # Taken from random.py code in Python 2.7.
    "shellReadMode": None,
    "shellWriteMode": None,
    "shuffle": False,
    "storageEngine": None,
    "storageEngineCacheSizeGB": None,
    "wiredTigerCollectionConfigString": None,
    "wiredTigerEngineConfigString": None,
    "wiredTigerIndexConfigString": None
}


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

# If set, then any jstests that have all of the specified tags will be excluded from the suite(s).
EXCLUDE_WITH_ALL_TAGS = None

# If set, then any jstests that have any of the specified tags will be excluded from the suite(s).
EXCLUDE_WITH_ANY_TAGS = None

# If true, then a test failure or error will cause resmoke.py to exit and not run any more tests.
FAIL_FAST = None

# If set, then only jstests that have all of the specified tags will be run during the jstest
# portion of the suite(s).
INCLUDE_WITH_ALL_TAGS = None

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

# If set, then resmoke.py will write out a report file with the status of each test that ran.
REPORT_FILE = None

# If set, then mongo shells started by resmoke.py will use the specified read mode.
SHELL_READ_MODE = None

# If set, then mongo shells started by resmoke.py will use the specified write mode.
SHELL_WRITE_MODE = None

# If true, then the order the tests run in is randomized. Otherwise the tests will run in
# alphabetical (case-insensitive) order.
SHUFFLE = None

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# storage engine.
STORAGE_ENGINE = None

# If set, then all mongod's started by resmoke.py and by the mongo shell will use the specified
# storage engine cache size.
STORAGE_ENGINE_CACHE_SIZE = None

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
EXTERNAL_SUITE_SELECTORS = [DEFAULT_UNIT_TEST_LIST,
                            DEFAULT_INTEGRATION_TEST_LIST,
                            DEFAULT_DBTEST_EXECUTABLE]

# This is used internally to store the executor name that is passed on the command line.
# Specifically it's used to record in the logs which executor a test is being run under.
INTERNAL_EXECUTOR_NAME = None
