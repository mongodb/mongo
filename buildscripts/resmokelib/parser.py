"""Parser for command line arguments."""

from __future__ import absolute_import

import collections
import os
import os.path

import datetime
import optparse

from . import config as _config
from . import utils
from .. import resmokeconfig

ResmokeConfig = collections.namedtuple(
    "ResmokeConfig",
    ["list_suites", "find_suites", "dry_run", "suite_files", "test_files", "logging_config"])


def _make_parser():  # pylint: disable=too-many-statements
    """Create and return the command line arguments parser."""
    parser = optparse.OptionParser()

    parser.add_option("--suites", dest="suite_files", metavar="SUITE1,SUITE2",
                      help=("Comma separated list of YAML files that each specify the configuration"
                            " of a suite. If the file is located in the resmokeconfig/suites/"
                            " directory, then the basename without the .yml extension can be"
                            " specified, e.g. 'core'. If a list of files is passed in as"
                            " positional arguments, they will be run using the suites'"
                            " configurations"))

    parser.add_option("--log", dest="logger_file", metavar="LOGGER",
                      help=("A YAML file that specifies the logging configuration. If the file is"
                            " located in the resmokeconfig/suites/ directory, then the basename"
                            " without the .yml extension can be specified, e.g. 'console'."))

    parser.add_option("--archiveFile", dest="archive_file", metavar="ARCHIVE_FILE",
                      help=("Sets the archive file name for the Evergreen task running the tests."
                            " The archive file is JSON format containing a list of tests that were"
                            " successfully archived to S3. If unspecified, no data files from tests"
                            " will be archived in S3. Tests can be designated for archival in the"
                            " task suite configuration file."))

    parser.add_option("--archiveLimitMb", type="int", dest="archive_limit_mb",
                      metavar="ARCHIVE_LIMIT_MB",
                      help=("Sets the limit (in MB) for archived files to S3. A value of 0"
                            " indicates there is no limit."))

    parser.add_option("--archiveLimitTests", type="int", dest="archive_limit_tests",
                      metavar="ARCHIVE_LIMIT_TESTS",
                      help=("Sets the maximum number of tests to archive to S3. A value"
                            " of 0 indicates there is no limit."))

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

    parser.add_option("--excludeWithAnyTags", action="append", dest="exclude_with_any_tags",
                      metavar="TAG1,TAG2",
                      help=("Comma separated list of tags. Any jstest that contains any of the"
                            " specified tags will be excluded from any suites that are run."))

    parser.add_option("-f", "--findSuites", action="store_true", dest="find_suites",
                      help="Lists the names of the suites that will execute the specified tests.")

    parser.add_option("--includeWithAnyTags", action="append", dest="include_with_any_tags",
                      metavar="TAG1,TAG2",
                      help=("Comma separated list of tags. For the jstest portion of the suite(s),"
                            " only tests which have at least one of the specified tags will be"
                            " run."))

    parser.add_option("-n", action="store_const", const="tests", dest="dry_run",
                      help="Outputs the tests that would be run.")

    # TODO: add support for --dryRun=commands
    parser.add_option("--dryRun", type="choice", action="store", dest="dry_run",
                      choices=("off", "tests"), metavar="MODE",
                      help=("Instead of running the tests, outputs the tests that would be run"
                            " (if MODE=tests). Defaults to MODE=%default."))

    parser.add_option("-j", "--jobs", type="int", dest="jobs", metavar="JOBS",
                      help=("The number of Job instances to use. Each instance will receive its"
                            " own MongoDB deployment to dispatch tests to."))

    parser.add_option("-l", "--listSuites", action="store_true", dest="list_suites",
                      help="Lists the names of the suites available to execute.")

    parser.add_option("--mongo", dest="mongo_executable", metavar="PATH",
                      help="The path to the mongo shell executable for resmoke.py to use.")

    parser.add_option("--mongod", dest="mongod_executable", metavar="PATH",
                      help="The path to the mongod executable for resmoke.py to use.")

    parser.add_option("--mongodSetParameters", dest="mongod_set_parameters",
                      metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
                      help=("Passes one or more --setParameter options to all mongod processes"
                            " started by resmoke.py. The argument is specified as bracketed YAML -"
                            " i.e. JSON with support for single quoted and unquoted keys."))

    parser.add_option("--mongos", dest="mongos_executable", metavar="PATH",
                      help="The path to the mongos executable for resmoke.py to use.")

    parser.add_option("--mongosSetParameters", dest="mongos_set_parameters",
                      metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
                      help=("Passes one or more --setParameter options to all mongos processes"
                            " started by resmoke.py. The argument is specified as bracketed YAML -"
                            " i.e. JSON with support for single quoted and unquoted keys."))

    parser.add_option("--nojournal", action="store_true", dest="no_journal",
                      help="Disables journaling for all mongod's.")

    parser.add_option("--nopreallocj", action="store_const", const="off", dest="prealloc_journal",
                      help="Disables preallocation of journal files for all mongod processes.")

    parser.add_option("--numClientsPerFixture", type="int", dest="num_clients_per_fixture",
                      help="Number of clients running tests per fixture.")

    parser.add_option("--perfReportFile", dest="perf_report_file", metavar="PERF_REPORT",
                      help="Writes a JSON file with performance test results.")

    parser.add_option("--preallocJournal", type="choice", action="store", dest="prealloc_journal",
                      choices=("on", "off"), metavar="ON|OFF",
                      help=("Enables or disables preallocation of journal files for all mongod"
                            " processes. Defaults to %default."))

    parser.add_option("--shellConnString", dest="shell_conn_string", metavar="CONN_STRING",
                      help="Overrides the default fixture and connect to an existing MongoDB"
                      " cluster instead. This is useful for connecting to a MongoDB"
                      " deployment started outside of resmoke.py including one running in a"
                      " debugger.")

    parser.add_option("--shellPort", dest="shell_port", metavar="PORT",
                      help="Convenience form of --shellConnString for connecting to an"
                      " existing MongoDB cluster with the URL mongodb://localhost:[PORT]."
                      " This is useful for connecting to a server running in a debugger.")

    parser.add_option("--repeat", type="int", dest="repeat", metavar="N",
                      help="Repeats the given suite(s) N times, or until one fails.")

    parser.add_option("--reportFailureStatus", type="choice", action="store",
                      dest="report_failure_status", choices=("fail",
                                                             "silentfail"), metavar="STATUS",
                      help="Controls if the test failure status should be reported as failed"
                      " or be silently ignored (STATUS=silentfail). Dynamic test failures will"
                      " never be silently ignored. Defaults to STATUS=%default.")

    parser.add_option("--reportFile", dest="report_file", metavar="REPORT",
                      help="Writes a JSON file with test status and timing information.")

    parser.add_option("--seed", type="int", dest="seed", metavar="SEED",
                      help=("Seed for the random number generator. Useful in combination with the"
                            " --shuffle option for producing a consistent test execution order."))

    parser.add_option("--serviceExecutor", dest="service_executor", metavar="EXECUTOR",
                      help="The service executor used by jstests")

    parser.add_option("--transportLayer", dest="transport_layer", metavar="TRANSPORT",
                      help="The transport layer used by jstests")

    parser.add_option("--shellReadMode", type="choice", action="store", dest="shell_read_mode",
                      choices=("commands", "compatibility", "legacy"), metavar="READ_MODE",
                      help="The read mode used by the mongo shell.")

    parser.add_option("--shellWriteMode", type="choice", action="store", dest="shell_write_mode",
                      choices=("commands", "compatibility", "legacy"), metavar="WRITE_MODE",
                      help="The write mode used by the mongo shell.")

    parser.add_option("--shuffle", action="store_const", const="on", dest="shuffle",
                      help=("Randomizes the order in which tests are executed. This is equivalent"
                            " to specifying --shuffleMode=on."))

    parser.add_option("--shuffleMode", type="choice", action="store", dest="shuffle",
                      choices=("on", "off", "auto"), metavar="ON|OFF|AUTO",
                      help=("Controls whether to randomize the order in which tests are executed."
                            " Defaults to auto when not supplied. auto enables randomization in"
                            " all cases except when the number of jobs requested is 1."))

    parser.add_option("--staggerJobs", type="choice", action="store", dest="stagger_jobs",
                      choices=("on", "off"), metavar="ON|OFF",
                      help=("Enables or disables the stagger of launching resmoke jobs."
                            " Defaults to %default."))

    parser.add_option("--storageEngine", dest="storage_engine", metavar="ENGINE",
                      help="The storage engine used by dbtests and jstests.")

    parser.add_option("--storageEngineCacheSizeGB", dest="storage_engine_cache_size_gb",
                      metavar="CONFIG", help="Sets the storage engine cache size configuration"
                      " setting for all mongod's.")

    parser.add_option("--tagFile", dest="tag_file", metavar="OPTIONS",
                      help="A YAML file that associates tests and tags.")

    parser.add_option("--wiredTigerCollectionConfigString", dest="wt_coll_config", metavar="CONFIG",
                      help="Sets the WiredTiger collection configuration setting for all mongod's.")

    parser.add_option("--wiredTigerEngineConfigString", dest="wt_engine_config", metavar="CONFIG",
                      help="Sets the WiredTiger engine configuration setting for all mongod's.")

    parser.add_option("--wiredTigerIndexConfigString", dest="wt_index_config", metavar="CONFIG",
                      help="Sets the WiredTiger index configuration setting for all mongod's.")

    parser.add_option("--executor", dest="executor_file",
                      help="OBSOLETE: Superceded by --suites; specify --suites=SUITE path/to/test"
                      " to run a particular test under a particular suite configuration.")

    evergreen_options = optparse.OptionGroup(
        parser, title="Evergreen options",
        description=("Options used to propagate information about the Evergreen task running this"
                     " script."))
    parser.add_option_group(evergreen_options)

    evergreen_options.add_option("--buildId", dest="build_id", metavar="BUILD_ID",
                                 help="Sets the build ID of the task.")

    evergreen_options.add_option("--distroId", dest="distro_id", metavar="DISTRO_ID",
                                 help=("Sets the identifier for the Evergreen distro running the"
                                       " tests."))

    evergreen_options.add_option("--executionNumber", type="int", dest="execution_number",
                                 metavar="EXECUTION_NUMBER",
                                 help=("Sets the number for the Evergreen execution running the"
                                       " tests."))

    evergreen_options.add_option("--gitRevision", dest="git_revision", metavar="GIT_REVISION",
                                 help=("Sets the git revision for the Evergreen task running the"
                                       " tests."))

    evergreen_options.add_option("--patchBuild", action="store_true", dest="patch_build",
                                 help=("Indicates that the Evergreen task running the tests is a"
                                       " patch build."))

    evergreen_options.add_option("--projectName", dest="project_name", metavar="PROJECT_NAME",
                                 help=("Sets the name of the Evergreen project running the tests."))

    evergreen_options.add_option("--revisionOrderId", dest="revision_order_id",
                                 metavar="REVISION_ORDER_ID",
                                 help="Sets the chronological order number of this commit.")

    evergreen_options.add_option("--taskName", dest="task_name", metavar="TASK_NAME",
                                 help="Sets the name of the Evergreen task running the tests.")

    evergreen_options.add_option("--taskId", dest="task_id", metavar="TASK_ID",
                                 help="Sets the Id of the Evergreen task running the tests.")

    evergreen_options.add_option("--variantName", dest="variant_name", metavar="VARIANT_NAME",
                                 help=("Sets the name of the Evergreen build variant running the"
                                       " tests."))

    evergreen_options.add_option("--versionId", dest="version_id", metavar="VERSION_ID",
                                 help="Sets the version ID of the task.")

    benchmark_options = optparse.OptionGroup(parser, title="Benchmark test options",
                                             description="Options for running Benchmark tests")

    parser.add_option_group(benchmark_options)

    benchmark_options.add_option("--benchmarkFilter", type="string", dest="benchmark_filter",
                                 metavar="BENCHMARK_FILTER",
                                 help="Regex to filter benchmark tests to run.")

    benchmark_options.add_option("--benchmarkListTests", dest="benchmark_list_tests",
                                 action="store_true", metavar="BENCHMARK_LIST_TESTS",
                                 help="Lists all benchmark test configurations in each test file.")

    benchmark_min_time_help = (
        "Minimum time to run each benchmark test for. Use this option instead of "
        "--benchmarkRepetitions to make a test run for a longer or shorter duration.")
    benchmark_options.add_option("--benchmarkMinTimeSecs", type="int",
                                 dest="benchmark_min_time_secs", metavar="BENCHMARK_MIN_TIME",
                                 help=benchmark_min_time_help)

    benchmark_repetitions_help = (
        "Set --benchmarkRepetitions=1 if you'd like to run the benchmark tests only once. By "
        "default, each test is run multiple times to provide statistics on the variance between "
        "runs; use --benchmarkMinTimeSecs if you'd like to run a test for a longer or shorter "
        "duration.")
    benchmark_options.add_option("--benchmarkRepetitions", type="int", dest="benchmark_repetitions",
                                 metavar="BENCHMARK_REPETITIONS", help=benchmark_repetitions_help)

    parser.set_defaults(logger_file="console", dry_run="off", find_suites=False, list_suites=False,
                        suite_files="with_server", prealloc_journal="off", shuffle="auto",
                        stagger_jobs="off")
    return parser


def parse_command_line():
    """Parse the command line arguments passed to resmoke.py."""
    parser = _make_parser()
    options, args = parser.parse_args()

    _validate_options(parser, options, args)
    _update_config_vars(options)

    return ResmokeConfig(list_suites=options.list_suites, find_suites=options.find_suites,
                         dry_run=options.dry_run, suite_files=options.suite_files.split(","),
                         test_files=args, logging_config=_get_logging_config(options.logger_file))


def _validate_options(parser, options, args):
    """Do preliminary validation on the options and error on any invalid options."""

    if options.shell_port is not None and options.shell_conn_string is not None:
        parser.error("Cannot specify both `shellPort` and `shellConnString`")

    if options.executor_file:
        parser.error("--executor is superseded by --suites; specify --suites={} {} to run the"
                     " test(s) under those suite configuration(s)".format(
                         options.executor_file, " ".join(args)))


def validate_benchmark_options():
    """Error out early if any options are incompatible with benchmark test suites.

    :return: None
    """

    if _config.REPEAT > 1:
        raise optparse.OptionValueError(
            "--repeat cannot be used with benchmark tests. Please use --benchmarkMinTimeSecs to "
            "increase the runtime of a single benchmark configuration.")

    if _config.JOBS > 1:
        raise optparse.OptionValueError(
            "--jobs=%d cannot be used for benchmark tests. Parallel jobs affect CPU cache access "
            "patterns and cause additional context switching, which lead to inaccurate benchmark "
            "results. Please use --jobs=1" % _config.JOBS)


def _update_config_vars(values):  # pylint: disable=too-many-statements
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

    _config.ARCHIVE_FILE = config.pop("archive_file")
    _config.ARCHIVE_LIMIT_MB = config.pop("archive_limit_mb")
    _config.ARCHIVE_LIMIT_TESTS = config.pop("archive_limit_tests")
    _config.BASE_PORT = int(config.pop("base_port"))
    _config.BUILDLOGGER_URL = config.pop("buildlogger_url")
    _config.DBPATH_PREFIX = _expand_user(config.pop("dbpath_prefix"))
    _config.DBTEST_EXECUTABLE = _expand_user(config.pop("dbtest_executable"))
    _config.DRY_RUN = config.pop("dry_run")
    _config.EXCLUDE_WITH_ANY_TAGS = _tags_from_list(config.pop("exclude_with_any_tags"))
    _config.FAIL_FAST = not config.pop("continue_on_failure")
    _config.INCLUDE_WITH_ANY_TAGS = _tags_from_list(config.pop("include_with_any_tags"))
    _config.JOBS = config.pop("jobs")
    _config.MONGO_EXECUTABLE = _expand_user(config.pop("mongo_executable"))
    _config.MONGOD_EXECUTABLE = _expand_user(config.pop("mongod_executable"))
    _config.MONGOD_SET_PARAMETERS = config.pop("mongod_set_parameters")
    _config.MONGOS_EXECUTABLE = _expand_user(config.pop("mongos_executable"))
    _config.MONGOS_SET_PARAMETERS = config.pop("mongos_set_parameters")
    _config.NO_JOURNAL = config.pop("no_journal")
    _config.NO_PREALLOC_JOURNAL = config.pop("prealloc_journal") == "off"
    _config.NUM_CLIENTS_PER_FIXTURE = config.pop("num_clients_per_fixture")
    _config.PERF_REPORT_FILE = config.pop("perf_report_file")
    _config.RANDOM_SEED = config.pop("seed")
    _config.REPEAT = config.pop("repeat")
    _config.REPORT_FAILURE_STATUS = config.pop("report_failure_status")
    _config.REPORT_FILE = config.pop("report_file")
    _config.SERVICE_EXECUTOR = config.pop("service_executor")
    _config.SHELL_READ_MODE = config.pop("shell_read_mode")
    _config.SHELL_WRITE_MODE = config.pop("shell_write_mode")
    _config.STAGGER_JOBS = config.pop("stagger_jobs") == "on"
    _config.STORAGE_ENGINE = config.pop("storage_engine")
    _config.STORAGE_ENGINE_CACHE_SIZE = config.pop("storage_engine_cache_size_gb")
    _config.TAG_FILE = config.pop("tag_file")
    _config.TRANSPORT_LAYER = config.pop("transport_layer")

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

    # Wiredtiger options.
    _config.WT_COLL_CONFIG = config.pop("wt_coll_config")
    _config.WT_ENGINE_CONFIG = config.pop("wt_engine_config")
    _config.WT_INDEX_CONFIG = config.pop("wt_index_config")

    # Benchmark options.
    _config.BENCHMARK_FILTER = config.pop("benchmark_filter")
    _config.BENCHMARK_LIST_TESTS = config.pop("benchmark_list_tests")
    benchmark_min_time = config.pop("benchmark_min_time_secs")
    if benchmark_min_time is not None:
        _config.BENCHMARK_MIN_TIME = datetime.timedelta(seconds=benchmark_min_time)
    _config.BENCHMARK_REPETITIONS = config.pop("benchmark_repetitions")

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
        _config.SHELL_CONN_STRING = conn_string

    if config:
        raise optparse.OptionValueError("Unknown option(s): %s" % (config.keys()))


def _get_logging_config(pathname):
    """Read YAML configuration from 'pathname' how to log tests and fixtures."""

    # Named loggers are specified as the basename of the file, without the .yml extension.
    if not utils.is_yaml_file(pathname) and not os.path.dirname(pathname):
        if pathname not in resmokeconfig.NAMED_LOGGERS:
            raise optparse.OptionValueError("Unknown logger '%s'" % pathname)
        pathname = resmokeconfig.NAMED_LOGGERS[pathname]  # Expand 'pathname' to full path.

    if not utils.is_yaml_file(pathname) or not os.path.isfile(pathname):
        raise optparse.OptionValueError("Expected a logger YAML config, but got '%s'" % pathname)

    return utils.load_yaml_file(pathname).pop("logging")


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
