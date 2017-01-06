"""
Parser for command line arguments.
"""

from __future__ import absolute_import

import collections
import os
import os.path
import optparse

from . import config as _config
from . import testing
from . import utils
from .. import resmokeconfig


# Mapping of the attribute of the parsed arguments (dest) to its key as it appears in the options
# YAML configuration file. Most should only be converting from snake_case to camelCase.
DEST_TO_CONFIG = {
    "base_port": "basePort",
    "buildlogger_url": "buildloggerUrl",
    "continue_on_failure": "continueOnFailure",
    "dbpath_prefix": "dbpathPrefix",
    "dbtest_executable": "dbtest",
    "dry_run": "dryRun",
    "exclude_with_all_tags": "excludeWithAllTags",
    "exclude_with_any_tags": "excludeWithAnyTags",
    "include_with_all_tags": "includeWithAllTags",
    "include_with_any_tags": "includeWithAnyTags",
    "jobs": "jobs",
    "mongo_executable": "mongo",
    "mongod_executable": "mongod",
    "mongod_parameters": "mongodSetParameters",
    "mongos_executable": "mongos",
    "mongos_parameters": "mongosSetParameters",
    "no_journal": "nojournal",
    "num_clients_per_fixture": "numClientsPerFixture",
    "prealloc_journal": "preallocJournal",
    "repeat": "repeat",
    "report_file": "reportFile",
    "seed": "seed",
    "shell_read_mode": "shellReadMode",
    "shell_write_mode": "shellWriteMode",
    "shuffle": "shuffle",
    "storage_engine": "storageEngine",
    "storage_engine_cache_size": "storageEngineCacheSizeGB",
    "wt_coll_config": "wiredTigerCollectionConfigString",
    "wt_engine_config": "wiredTigerEngineConfigString",
    "wt_index_config": "wiredTigerIndexConfigString"
}


def parse_command_line():
    """
    Parses the command line arguments passed to resmoke.py.
    """

    parser = optparse.OptionParser()

    parser.add_option("--suites", dest="suite_files", metavar="SUITE1,SUITE2",
                      help=("Comma separated list of YAML files that each specify the configuration"
                            " of a suite. If the file is located in the resmokeconfig/suites/"
                            " directory, then the basename without the .yml extension can be"
                            " specified, e.g. 'core'."))

    parser.add_option("--executor", dest="executor_file", metavar="EXECUTOR",
                      help=("A YAML file that specifies the executor configuration. If the file is"
                            " located in the resmokeconfig/suites/ directory, then the basename"
                            " without the .yml extension can be specified, e.g. 'core_small_oplog'."
                            " If specified in combination with the --suites option, then the suite"
                            " configuration takes precedence."))

    parser.add_option("--log", dest="logger_file", metavar="LOGGER",
                      help=("A YAML file that specifies the logging configuration. If the file is"
                            " located in the resmokeconfig/suites/ directory, then the basename"
                            " without the .yml extension can be specified, e.g. 'console'."))

    parser.add_option("--options", dest="options_file", metavar="OPTIONS",
                      help="A YAML file that specifies global options to resmoke.py.")

    parser.add_option("--basePort", dest="base_port", metavar="PORT",
                      help=("The starting port number to use for mongod and mongos processes"
                            " spawned by resmoke.py or the tests themselves. Each fixture and Job"
                            " allocates a contiguous range of ports."))

    parser.add_option("--buildloggerUrl", action="store", dest="buildlogger_url", metavar="URL",
                      help="The root url of the buildlogger server.")

    parser.add_option("--continueOnFailure", action="store_true", dest="continue_on_failure",
                      help="Executes all tests in all suites, even if some of them fail.")

    parser.add_option("--dbpathPrefix", dest="dbpath_prefix", metavar="PATH",
                      help=("The directory which will contain the dbpaths of any mongod's started"
                            " by resmoke.py or the tests themselves."))

    parser.add_option("--dbtest", dest="dbtest_executable", metavar="PATH",
                      help="The path to the dbtest executable for resmoke to use.")

    parser.add_option("--excludeWithAllTags", action="append", dest="exclude_with_all_tags",
                      metavar="TAG1,TAG2",
                      help=("Comma separated list of tags. Any jstest that contains all of the"
                            " specified tags will be excluded from any suites that are run."))

    parser.add_option("--excludeWithAnyTags", action="append", dest="exclude_with_any_tags",
                      metavar="TAG1,TAG2",
                      help=("Comma separated list of tags. Any jstest that contains any of the"
                            " specified tags will be excluded from any suites that are run."))

    parser.add_option("-f", "--findSuites", action="store_true", dest="find_suites",
                      help="List the names of the suites that will execute the specified tests.")

    parser.add_option("--includeWithAllTags", action="append", dest="include_with_all_tags",
                      metavar="TAG1,TAG2",
                      help=("Comma separated list of tags. For the jstest portion of the suite(s),"
                            " only tests which have all of the specified tags will be run."))

    parser.add_option("--includeWithAnyTags", action="append", dest="include_with_any_tags",
                      metavar="TAG1,TAG2",
                      help=("Comma separated list of tags. For the jstest portion of the suite(s),"
                            " only tests which have at least one of the specified tags will be"
                            " run."))

    parser.add_option("-n", action="store_const", const="tests", dest="dry_run",
                      help=("Output the tests that would be run."))

    # TODO: add support for --dryRun=commands
    parser.add_option("--dryRun", type="choice", action="store", dest="dry_run",
                      choices=("off", "tests"), metavar="MODE",
                      help=("Instead of running the tests, output the tests that would be run"
                            " (if MODE=tests). Defaults to MODE=%default."))

    parser.add_option("-j", "--jobs", type="int", dest="jobs", metavar="JOBS",
                      help=("The number of Job instances to use. Each instance will receive its own"
                            " MongoDB deployment to dispatch tests to."))

    parser.add_option("-l", "--listSuites", action="store_true", dest="list_suites",
                      help="List the names of the suites available to execute.")

    parser.add_option("--mongo", dest="mongo_executable", metavar="PATH",
                      help="The path to the mongo shell executable for resmoke.py to use.")

    parser.add_option("--mongod", dest="mongod_executable", metavar="PATH",
                      help="The path to the mongod executable for resmoke.py to use.")

    parser.add_option("--mongodSetParameters", dest="mongod_parameters",
                      metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
                      help=("Pass one or more --setParameter options to all mongod processes"
                            " started by resmoke.py. The argument is specified as bracketed YAML -"
                            " i.e. JSON with support for single quoted and unquoted keys."))

    parser.add_option("--mongos", dest="mongos_executable", metavar="PATH",
                      help="The path to the mongos executable for resmoke.py to use.")

    parser.add_option("--mongosSetParameters", dest="mongos_parameters",
                      metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
                      help=("Pass one or more --setParameter options to all mongos processes"
                            " started by resmoke.py. The argument is specified as bracketed YAML -"
                            " i.e. JSON with support for single quoted and unquoted keys."))

    parser.add_option("--nojournal", action="store_true", dest="no_journal",
                      help="Disable journaling for all mongod's.")

    parser.add_option("--nopreallocj", action="store_const", const="off", dest="prealloc_journal",
                      help="Disable preallocation of journal files for all mongod processes.")

    parser.add_option("--numClientsPerFixture", type="int", dest="num_clients_per_fixture",
                      help="Number of clients running tests per fixture")

    parser.add_option("--preallocJournal", type="choice", action="store", dest="prealloc_journal",
                      choices=("on", "off"), metavar="ON|OFF",
                      help=("Enable or disable preallocation of journal files for all mongod"
                            " processes. Defaults to %default."))

    parser.add_option("--repeat", type="int", dest="repeat", metavar="N",
                      help="Repeat the given suite(s) N times, or until one fails.")

    parser.add_option("--reportFile", dest="report_file", metavar="REPORT",
                      help="Write a JSON file with test status and timing information.")

    parser.add_option("--seed", type="int", dest="seed", metavar="SEED",
                      help=("Seed for the random number generator. Useful in combination with the"
                            " --shuffle option for producing a consistent test execution order."))

    parser.add_option("--shellReadMode", type="choice", action="store", dest="shell_read_mode",
                      choices=("commands", "compatibility", "legacy"), metavar="READ_MODE",
                      help="The read mode used by the mongo shell.")

    parser.add_option("--shellWriteMode", type="choice", action="store", dest="shell_write_mode",
                      choices=("commands", "compatibility", "legacy"), metavar="WRITE_MODE",
                      help="The write mode used by the mongo shell.")

    parser.add_option("--shuffle", action="store_true", dest="shuffle",
                      help="Randomize the order in which tests are executed.")

    parser.add_option("--storageEngine", dest="storage_engine", metavar="ENGINE",
                      help="The storage engine used by dbtests and jstests.")

    parser.add_option("--storageEngineCacheSizeGB", dest="storage_engine_cache_size",
                      metavar="CONFIG", help="Set the storage engine cache size configuration"
                      " setting for all mongod's.")

    parser.add_option("--wiredTigerCollectionConfigString", dest="wt_coll_config", metavar="CONFIG",
                      help="Set the WiredTiger collection configuration setting for all mongod's.")

    parser.add_option("--wiredTigerEngineConfigString", dest="wt_engine_config", metavar="CONFIG",
                      help="Set the WiredTiger engine configuration setting for all mongod's.")

    parser.add_option("--wiredTigerIndexConfigString", dest="wt_index_config", metavar="CONFIG",
                      help="Set the WiredTiger index configuration setting for all mongod's.")

    parser.set_defaults(executor_file="with_server",
                        logger_file="console",
                        dry_run="off",
                        find_suites=False,
                        list_suites=False,
                        prealloc_journal="off")

    return parser.parse_args()


def get_logging_config(values):
    return _get_logging_config(values.logger_file)


def update_config_vars(values):
    options = _get_options_config(values.options_file)

    config = _config.DEFAULTS.copy()
    config.update(options)

    values = vars(values)
    for dest in values:
        if dest not in DEST_TO_CONFIG:
            continue
        config_var = DEST_TO_CONFIG[dest]
        if values[dest] is not None:
            config[config_var] = values[dest]

    _config.BASE_PORT = int(config.pop("basePort"))
    _config.BUILDLOGGER_URL = config.pop("buildloggerUrl")
    _config.DBPATH_PREFIX = _expand_user(config.pop("dbpathPrefix"))
    _config.DBTEST_EXECUTABLE = _expand_user(config.pop("dbtest"))
    _config.DRY_RUN = config.pop("dryRun")
    _config.EXCLUDE_WITH_ALL_TAGS = config.pop("excludeWithAllTags")
    _config.EXCLUDE_WITH_ANY_TAGS = config.pop("excludeWithAnyTags")
    _config.FAIL_FAST = not config.pop("continueOnFailure")
    _config.INCLUDE_WITH_ALL_TAGS = config.pop("includeWithAllTags")
    _config.INCLUDE_WITH_ANY_TAGS = config.pop("includeWithAnyTags")
    _config.JOBS = config.pop("jobs")
    _config.MONGO_EXECUTABLE = _expand_user(config.pop("mongo"))
    _config.MONGOD_EXECUTABLE = _expand_user(config.pop("mongod"))
    _config.MONGOD_SET_PARAMETERS = config.pop("mongodSetParameters")
    _config.MONGOS_EXECUTABLE = _expand_user(config.pop("mongos"))
    _config.MONGOS_SET_PARAMETERS = config.pop("mongosSetParameters")
    _config.NO_JOURNAL = config.pop("nojournal")
    _config.NO_PREALLOC_JOURNAL = config.pop("preallocJournal") == "off"
    _config.NUM_CLIENTS_PER_FIXTURE = config.pop("numClientsPerFixture")
    _config.RANDOM_SEED = config.pop("seed")
    _config.REPEAT = config.pop("repeat")
    _config.REPORT_FILE = config.pop("reportFile")
    _config.SHELL_READ_MODE = config.pop("shellReadMode")
    _config.SHELL_WRITE_MODE = config.pop("shellWriteMode")
    _config.SHUFFLE = config.pop("shuffle")
    _config.STORAGE_ENGINE = config.pop("storageEngine")
    _config.STORAGE_ENGINE_CACHE_SIZE = config.pop("storageEngineCacheSizeGB")
    _config.WT_COLL_CONFIG = config.pop("wiredTigerCollectionConfigString")
    _config.WT_ENGINE_CONFIG = config.pop("wiredTigerEngineConfigString")
    _config.WT_INDEX_CONFIG = config.pop("wiredTigerIndexConfigString")

    if config:
        raise optparse.OptionValueError("Unknown option(s): %s" % (config.keys()))


def create_test_membership_map(fail_on_missing_selector=False):
    """
    Returns a dict keyed by test name containing all of the suites that will run that test.
    Since this iterates through every available suite, it should only be run once.
    """

    test_membership = collections.defaultdict(list)
    suite_names = get_named_suites()
    for suite_name in suite_names:
        try:
            suite_config = _get_suite_config(suite_name)
            suite = testing.suite.Suite(suite_name, suite_config)
        except IOError as err:
            # If unittests.txt or integration_tests.txt aren't there we'll ignore the error because
            # unittests haven't been built yet (this is highly likely using find interactively).
            if err.filename in _config.EXTERNAL_SUITE_SELECTORS:
                if not fail_on_missing_selector:
                    continue
            raise

        for group in suite.test_groups:
            for testfile in group.tests:
                if isinstance(testfile, dict):
                    continue
                test_membership[testfile].append(suite_name)
    return test_membership


def get_suites(values, args):
    if (values.suite_files is None and not args) or (values.suite_files is not None and args):
        raise optparse.OptionValueError("Must specify either --suites or a list of tests")

    _config.INTERNAL_EXECUTOR_NAME = values.executor_file

    # If there are no suites specified, but there are args, assume they are jstests.
    if args:
        # Do not change the execution order of the jstests passed as args, unless a tag option is
        # specified. If an option is specified, then sort the tests for consistent execution order.
        _config.ORDER_TESTS_BY_NAME = any(tag_filter is not None for
                                          tag_filter in (_config.EXCLUDE_WITH_ALL_TAGS,
                                                         _config.EXCLUDE_WITH_ANY_TAGS,
                                                         _config.INCLUDE_WITH_ALL_TAGS,
                                                         _config.INCLUDE_WITH_ANY_TAGS))
        # No specified config, just use the following, and default the logging and executor.
        suite_config = _make_jstests_config(args)
        _ensure_executor(suite_config, values.executor_file)
        suite = testing.suite.Suite("<jstests>", suite_config)
        return [suite]

    suite_files = values.suite_files.split(",")

    suites = []
    for suite_filename in suite_files:
        suite_config = _get_suite_config(suite_filename)
        _ensure_executor(suite_config, values.executor_file)
        suite = testing.suite.Suite(suite_filename, suite_config)
        suites.append(suite)
    return suites


def get_named_suites():
    """
    Returns the list of suites available to execute.
    """

    # Skip "with_server" and "no_server" because they do not define any test files to run.
    executor_only = set(["with_server", "no_server"])
    suite_names = [suite for suite in resmokeconfig.NAMED_SUITES if suite not in executor_only]
    suite_names.sort()
    return suite_names


def _get_logging_config(pathname):
    """
    Attempts to read a YAML configuration from 'pathname' that describes
    how resmoke.py should log the tests and fixtures.
    """

    # Named loggers are specified as the basename of the file, without the .yml extension.
    if not utils.is_yaml_file(pathname) and not os.path.dirname(pathname):
        if pathname not in resmokeconfig.NAMED_LOGGERS:
            raise optparse.OptionValueError("Unknown logger '%s'" % (pathname))
        pathname = resmokeconfig.NAMED_LOGGERS[pathname]  # Expand 'pathname' to full path.

    if not utils.is_yaml_file(pathname) or not os.path.isfile(pathname):
        raise optparse.OptionValueError("Expected a logger YAML config, but got '%s'" % (pathname))

    return utils.load_yaml_file(pathname).pop("logging")


def _get_options_config(pathname):
    """
    Attempts to read a YAML configuration from 'pathname' that describes
    any modifications to global options.
    """

    if pathname is None:
        return {}

    return utils.load_yaml_file(pathname).pop("options")


def _get_suite_config(pathname):
    """
    Attempts to read a YAML configuration from 'pathname' that describes
    what tests to run and how to run them.
    """

    # Named suites are specified as the basename of the file, without the .yml extension.
    if not utils.is_yaml_file(pathname) and not os.path.dirname(pathname):
        if pathname not in resmokeconfig.NAMED_SUITES:
            raise optparse.OptionValueError("Unknown suite '%s'" % (pathname))
        pathname = resmokeconfig.NAMED_SUITES[pathname]  # Expand 'pathname' to full path.

    if not utils.is_yaml_file(pathname) or not os.path.isfile(pathname):
        raise optparse.OptionValueError("Expected a suite YAML config, but got '%s'" % (pathname))

    return utils.load_yaml_file(pathname)


def _make_jstests_config(js_files):
    for pathname in js_files:
        if not utils.is_js_file(pathname) or not os.path.isfile(pathname):
            raise optparse.OptionValueError("Expected a list of JS files, but got '%s'"
                                            % (pathname))

    return {"selector": {"js_test": {"roots": js_files}}}


def _ensure_executor(suite_config, executor_pathname):
    if "executor" not in suite_config:
        # Named executors are specified as the basename of the file, without the .yml extension.
        if not utils.is_yaml_file(executor_pathname) and not os.path.dirname(executor_pathname):
            if executor_pathname not in resmokeconfig.NAMED_SUITES:
                raise optparse.OptionValueError("Unknown executor '%s'" % (executor_pathname))
            executor_pathname = resmokeconfig.NAMED_SUITES[executor_pathname]

        if not utils.is_yaml_file(executor_pathname) or not os.path.isfile(executor_pathname):
            raise optparse.OptionValueError("Expected an executor YAML config, but got '%s'"
                                            % (executor_pathname))

        suite_config["executor"] = utils.load_yaml_file(executor_pathname).pop("executor")


def _expand_user(pathname):
    """
    Wrapper around os.path.expanduser() to do nothing when given None.
    """
    if pathname is None:
        return None
    return os.path.expanduser(pathname)
