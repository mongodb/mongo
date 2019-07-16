#!/usr/bin/env python3
"""Command line utility for determining what jstests have been added or modified."""

import collections
import copy
import json
import logging
import optparse
import os.path
import subprocess
import shlex
import sys
import urllib.parse

from git import Repo
import structlog
from structlog.stdlib import LoggerFactory
import yaml

from shrub.config import Configuration
from shrub.command import CommandDefinition
from shrub.task import TaskDependency
from shrub.variant import DisplayTaskDefinition
from shrub.variant import TaskSpec

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.patch_builds.change_data import find_changed_files
from buildscripts import resmokelib
from buildscripts.ciconfig import evergreen
# pylint: enable=wrong-import-position

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.getLogger(__name__)
EXTERNAL_LOGGERS = {
    "evergreen",
    "git",
    "urllib3",
}

AVG_TEST_RUNTIME_ANALYSIS_DAYS = 14
AVG_TEST_TIME_MULTIPLIER = 3
CONFIG_FILE = ".evergreen.yml"
REPEAT_SUITES = 2
EVERGREEN_FILE = "etc/evergreen.yml"
MAX_TASKS_TO_CREATE = 1000
# The executor_file and suite_files defaults are required to make the suite resolver work
# correctly.
SELECTOR_FILE = "etc/burn_in_tests.yml"
SUITE_FILES = ["with_server"]

SUPPORTED_TEST_KINDS = ("fsm_workload_test", "js_test", "json_schema_test",
                        "multi_stmt_txn_passthrough", "parallel_fsm_workload_test")

BURN_IN_TESTS_GEN_TASK = "burn_in_tests_gen"
BURN_IN_TESTS_TASK = "burn_in_tests"


def parse_command_line():
    """Parse command line options."""

    parser = optparse.OptionParser(usage="Usage: %prog [options] [resmoke command]")

    parser.add_option(
        "--maxRevisions", dest="max_revisions", type=int, default=25,
        help=("Maximum number of revisions to check for changes. Default is"
              " %default."))

    parser.add_option(
        "--branch", dest="branch", default="master",
        help=("The name of the branch the working branch was based on. Default is"
              " '%default'."))

    parser.add_option("--baseCommit", dest="base_commit", default=None,
                      help="The base commit to compare to for determining changes.")

    parser.add_option(
        "--buildVariant", dest="buildvariant", default=None,
        help=("The buildvariant to select the tasks. Required when"
              " generating the JSON file with test executor information"))

    parser.add_option(
        "--runBuildVariant", dest="run_buildvariant", default=None,
        help=("The buildvariant the tasks will execute on. If not specified then tasks"
              " will execute on the the buildvariant specified in --buildVariant."))

    parser.add_option(
        "--distro", dest="distro", default=None,
        help=("The distro the tasks will execute on. Can only be specified"
              " with --generateTasksFile."))

    parser.add_option(
        "--checkEvergreen", dest="check_evergreen", default=False, action="store_true",
        help=("Checks Evergreen for the last commit that was scheduled."
              " This way all the tests that haven't been burned in will be run."))

    parser.add_option(
        "--generateTasksFile", dest="generate_tasks_file", default=None,
        help=("Write an Evergreen generate.tasks JSON file. If this option is"
              " specified then no tests will be executed."))

    parser.add_option("--noExec", dest="no_exec", default=False, action="store_true",
                      help="Do not run resmoke loop on new tests.")

    parser.add_option("--reportFile", dest="report_file", default="report.json",
                      help="Write a JSON file with test results. Default is '%default'.")

    parser.add_option("--testListFile", dest="test_list_file", default=None, metavar="TESTLIST",
                      help="Load a JSON file with tests to run.")

    parser.add_option("--testListOutfile", dest="test_list_outfile", default=None,
                      help="Write a JSON file with test executor information.")

    parser.add_option(
        "--repeatTests", dest="repeat_tests_num", default=None, type=int,
        help="The number of times to repeat each test. If --repeatTestsSecs is not"
        " specified then this will be set to {}.".format(REPEAT_SUITES))

    parser.add_option(
        "--repeatTestsMin", dest="repeat_tests_min", default=None, type=int,
        help="The minimum number of times to repeat each test when --repeatTestsSecs"
        " is specified.")

    parser.add_option(
        "--repeatTestsMax", dest="repeat_tests_max", default=None, type=int,
        help="The maximum number of times to repeat each test when --repeatTestsSecs"
        " is specified.")

    parser.add_option(
        "--repeatTestsSecs", dest="repeat_tests_secs", default=None, type=float,
        help="Time, in seconds, to repeat each test. Note that this option is"
        " mutually exclusive with with --repeatTests.")

    # This disables argument parsing on the first unrecognized parameter. This allows us to pass
    # a complete resmoke.py command line without accidentally parsing its options.
    parser.disable_interspersed_args()

    options, args = parser.parse_args()
    validate_options(parser, options)

    return options, args


def check_variant(buildvariant, parser):
    """Check if the buildvariant is found in the evergreen file."""
    evg_conf = evergreen.parse_evergreen_file(EVERGREEN_FILE)
    if not evg_conf.get_variant(buildvariant):
        parser.error("Buildvariant '{}' not found in {}, select from:\n\t{}".format(
            buildvariant, EVERGREEN_FILE, "\n\t".join(sorted(evg_conf.variant_names))))


def validate_options(parser, options):
    """Validate command line options."""

    if options.repeat_tests_max:
        if options.repeat_tests_secs is None:
            parser.error("Must specify --repeatTestsSecs with --repeatTestsMax")

        if options.repeat_tests_min and options.repeat_tests_min > options.repeat_tests_max:
            parser.error("--repeatTestsSecsMin is greater than --repeatTestsMax")

    if options.repeat_tests_min and options.repeat_tests_secs is None:
        parser.error("Must specify --repeatTestsSecs with --repeatTestsMin")

    if options.repeat_tests_num and options.repeat_tests_secs:
        parser.error("Cannot specify --repeatTests and --repeatTestsSecs")

    if options.test_list_file is None and options.buildvariant is None:
        parser.error("Must specify --buildVariant to find changed tests")

    if options.buildvariant:
        check_variant(options.buildvariant, parser)

    if options.run_buildvariant:
        check_variant(options.run_buildvariant, parser)


def _is_file_a_test_file(file_path):
    """
    Check if the given path points to a test file.

    :param file_path: path to file.
    :return: True if path points to test.
    """
    # Check that the file exists because it may have been moved or deleted in the patch.
    if os.path.splitext(file_path)[1] != ".js" or not os.path.isfile(file_path):
        return False

    if "jstests" not in file_path:
        return False

    return True


def find_changed_tests(repo: Repo):
    """
    Find the changed tests.

    Use git to find which files have changed in this patch.
    TODO: This should be expanded to search for enterprise modules.
    The returned file paths are in normalized form (see os.path.normpath(path)).

    :returns: Set of changed tests.
    """
    changed_files = find_changed_files(repo)
    LOGGER.debug("Found changed files", files=changed_files)
    changed_tests = {os.path.normpath(path) for path in changed_files if _is_file_a_test_file(path)}
    LOGGER.debug("Found changed tests", files=changed_tests)
    return changed_tests


def find_excludes(selector_file):
    """Parse etc/burn_in_tests.yml. Returns lists of excluded suites, tasks & tests."""

    if not selector_file:
        return ([], [], [])

    with open(selector_file, "r") as fstream:
        yml = yaml.load(fstream)

    try:
        js_test = yml["selector"]["js_test"]
    except KeyError:
        raise Exception("The selector file " + selector_file +
                        " is missing the 'selector.js_test' key")

    return (resmokelib.utils.default_if_none(js_test.get("exclude_suites"), []),
            resmokelib.utils.default_if_none(js_test.get("exclude_tasks"), []),
            resmokelib.utils.default_if_none(js_test.get("exclude_tests"), []))


def filter_tests(tests, exclude_tests):
    """Exclude tests which have been blacklisted.

    A test is in the tests list, i.e., ['jstests/core/a.js']
    The tests paths must be in normalized form (see os.path.normpath(path)).
    """

    if not exclude_tests or not tests:
        return tests

    # The exclude_tests can be specified using * and ** to specify directory and file patterns.
    excluded_globbed = set()
    for exclude_test_pattern in exclude_tests:
        excluded_globbed.update(resmokelib.utils.globstar.iglob(exclude_test_pattern))

    return set(tests) - excluded_globbed


def create_executor_list(suites, exclude_suites):
    """Create the executor list.

    Looks up what other resmoke suites run the tests specified in the suites
    parameter. Returns a dict keyed by suite name / executor, value is tests
    to run under that executor.
    """

    test_membership = resmokelib.suitesconfig.create_test_membership_map(
        test_kind=SUPPORTED_TEST_KINDS)

    memberships = collections.defaultdict(list)
    for suite in suites:
        for test in suite.tests:
            for executor in set(test_membership[test]) - set(exclude_suites):
                if test not in memberships[executor]:
                    memberships[executor].append(test)
    return memberships


def _get_task_name(task):
    """Return the task var from a "generate resmoke task" instead of the task name."""

    if task.is_generate_resmoke_task:
        return task.generated_task_name

    return task.name


def _set_resmoke_args(task):
    """Set the resmoke args to include the --suites option.

    The suite name from "generate resmoke tasks" can be specified as a var or directly in the
    resmoke_args.
    """

    resmoke_args = task.combined_resmoke_args
    suite_name = evergreen.ResmokeArgs.get_arg(resmoke_args, "suites")
    if task.is_generate_resmoke_task:
        suite_name = task.get_vars_suite_name(task.generate_resmoke_tasks_command["vars"])

    return evergreen.ResmokeArgs.get_updated_arg(resmoke_args, "suites", suite_name)


def create_task_list(  #pylint: disable=too-many-locals
        evergreen_conf, buildvariant, suites, exclude_tasks):
    """Find associated tasks for the specified buildvariant and suites.

    Returns a dict keyed by task_name, with executor, resmoke_args & tests, i.e.,
    {'jsCore_small_oplog':
        {'resmoke_args': '--suites=core_small_oplog --storageEngine=inMemory',
         'tests': ['jstests/core/all2.js', 'jstests/core/all3.js'],
         'use_multiversion': '/data/multiversion'}
    }
    """

    evg_buildvariant = evergreen_conf.get_variant(buildvariant)
    if not evg_buildvariant:
        print("Buildvariant '{}' not found in {}".format(buildvariant, evergreen_conf.path))
        sys.exit(1)

    # Find all the buildvariant tasks.
    exclude_tasks_set = set(exclude_tasks)
    variant_task = {
        _get_task_name(task): task
        for task in evg_buildvariant.tasks
        if task.name not in exclude_tasks_set and task.combined_resmoke_args
    }

    # Return the list of tasks to run for the specified suite.
    return {
        task_name: {
            "resmoke_args": _set_resmoke_args(task), "tests": suites[task.resmoke_suite],
            "use_multiversion": task.multiversion_path
        }
        for task_name, task in variant_task.items() if task.resmoke_suite in suites
    }


def _write_json_file(json_data, pathname):
    """Write out a JSON file."""

    with open(pathname, "w") as fstream:
        json.dump(json_data, fstream, indent=4)


def _load_tests_file(pathname):
    """Load the list of tests and executors from the specified file.

    The file might not exist, and this is fine. The task running this becomes a noop.
    """

    if not os.path.isfile(pathname):
        return None
    with open(pathname, "r") as fstream:
        return json.load(fstream)


def _update_report_data(data_to_update, pathname, task):
    """Read in the report file from the previous resmoke.py run, if it exists.

    We'll concat it to the data_to_update dict.
    """

    if not os.path.isfile(pathname):
        return

    with open(pathname, "r") as fstream:
        report_data = json.load(fstream)

    for result in report_data["results"]:
        result["test_file"] += ":" + task

    data_to_update["failures"] += report_data["failures"]
    data_to_update["results"] += report_data["results"]


def get_resmoke_repeat_options(options):
    """Build the resmoke repeat options."""

    if options.repeat_tests_secs:
        repeat_options = "--repeatTestsSecs={}".format(options.repeat_tests_secs)
        if options.repeat_tests_min:
            repeat_options += " --repeatTestsMin={}".format(options.repeat_tests_min)
        if options.repeat_tests_max:
            repeat_options += " --repeatTestsMax={}".format(options.repeat_tests_max)
    else:
        # To maintain previous default behavior, we set repeat_suites to 2 if
        # options.repeat_tests_secs and options.repeat_tests_num are both not specified.
        repeat_suites = options.repeat_tests_num if options.repeat_tests_num else REPEAT_SUITES
        repeat_options = "--repeatSuites={}".format(repeat_suites)

    return repeat_options


def _set_resmoke_cmd(options, args):
    """Build the resmoke command, if a resmoke.py command wasn't passed in."""

    new_args = copy.deepcopy(args) if args else [sys.executable, "buildscripts/resmoke.py"]
    new_args += get_resmoke_repeat_options(options).split()

    return new_args


def _sub_task_name(options, task, task_num):
    """Return the generated sub-task name."""
    task_name_prefix = options.buildvariant
    if options.run_buildvariant:
        task_name_prefix = options.run_buildvariant
    return "burn_in:{}_{}_{}".format(task_name_prefix, task, task_num)


def _get_run_buildvariant(options):
    """Return the build variant to execute the tasks on."""
    if options.run_buildvariant:
        return options.run_buildvariant
    return options.buildvariant


def create_generate_tasks_file(options, tests_by_task):
    """Create the Evergreen generate.tasks file."""

    # pylint: disable=too-many-locals

    evg_config = Configuration()
    task_specs = []
    task_names = [BURN_IN_TESTS_GEN_TASK]
    for task in sorted(tests_by_task):
        multiversion_path = tests_by_task[task].get("use_multiversion")
        for test_num, test in enumerate(tests_by_task[task]["tests"]):
            sub_task_name = _sub_task_name(options, task, test_num)
            task_names.append(sub_task_name)
            evg_sub_task = evg_config.task(sub_task_name)
            evg_sub_task.dependency(TaskDependency("compile"))
            task_spec = TaskSpec(sub_task_name)
            if options.distro:
                task_spec.distro(options.distro)
            task_specs.append(task_spec)
            run_tests_vars = {
                "resmoke_args":
                    "{} {} {}".format(tests_by_task[task]["resmoke_args"],
                                      get_resmoke_repeat_options(options), test),
            }
            commands = []
            commands.append(CommandDefinition().function("do setup"))
            if multiversion_path:
                run_tests_vars["task_path_suffix"] = multiversion_path
                commands.append(CommandDefinition().function("do multiversion setup"))
            commands.append(CommandDefinition().function("run tests").vars(run_tests_vars))
            evg_sub_task.commands(commands)

    display_task = DisplayTaskDefinition(BURN_IN_TESTS_TASK).execution_tasks(task_names)
    evg_config.variant(_get_run_buildvariant(options)).tasks(task_specs).display_task(display_task)

    json_config = evg_config.to_map()
    tasks_to_create = len(json_config.get('tasks', []))
    if tasks_to_create > MAX_TASKS_TO_CREATE:
        LOGGER.warning("Attempting to create more tasks than max, aborting", tasks=tasks_to_create)
        sys.exit(1)
    _write_json_file(json_config, options.generate_tasks_file)


def run_tests(no_exec, tests_by_task, resmoke_cmd, report_file):
    """Run the tests if not in no_exec mode."""

    if no_exec:
        return

    test_results = {"failures": 0, "results": []}

    for task in sorted(tests_by_task):
        new_resmoke_cmd = copy.deepcopy(resmoke_cmd)
        new_resmoke_cmd.extend(shlex.split(tests_by_task[task]["resmoke_args"]))
        new_resmoke_cmd.extend(tests_by_task[task]["tests"])
        try:
            subprocess.check_call(new_resmoke_cmd, shell=False)
        except subprocess.CalledProcessError as err:
            print("Resmoke returned an error with task:", task)
            _update_report_data(test_results, report_file, task)
            _write_json_file(test_results, report_file)
            sys.exit(err.returncode)

        # Note - _update_report_data concatenates to test_results the current results to the
        # previously saved results.
        _update_report_data(test_results, report_file, task)

    _write_json_file(test_results, report_file)


def configure_logging():
    """Configure logging for the application."""

    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=logging.DEBUG,
        stream=sys.stdout,
    )
    for log_name in EXTERNAL_LOGGERS:
        logging.getLogger(log_name).setLevel(logging.WARNING)


def main():
    """Execute Main program."""

    configure_logging()
    options, args = parse_command_line()
    resmoke_cmd = _set_resmoke_cmd(options, args)

    # Load the dict of tests to run.
    if options.test_list_file:
        tests_by_task = _load_tests_file(options.test_list_file)
        # If there are no tests to run, carry on.
        if tests_by_task is None:
            test_results = {"failures": 0, "results": []}
            _write_json_file(test_results, options.report_file)
            sys.exit(0)

    # Run the executor finder.
    else:
        # Parse the Evergreen project configuration file.
        evergreen_conf = evergreen.parse_evergreen_file(EVERGREEN_FILE)

        repo = Repo(".")
        changed_tests = find_changed_tests(repo)
        exclude_suites, exclude_tasks, exclude_tests = find_excludes(SELECTOR_FILE)
        changed_tests = filter_tests(changed_tests, exclude_tests)

        if changed_tests:
            suites = resmokelib.suitesconfig.get_suites(suite_files=SUITE_FILES,
                                                        test_files=changed_tests)
            tests_by_executor = create_executor_list(suites, exclude_suites)
            tests_by_task = create_task_list(evergreen_conf, options.buildvariant,
                                             tests_by_executor, exclude_tasks)
        else:
            print("No new or modified tests found.")
            tests_by_task = {}

        if options.test_list_outfile:
            _write_json_file(tests_by_task, options.test_list_outfile)

    if options.generate_tasks_file:
        create_generate_tasks_file(options, tests_by_task)
    else:
        run_tests(options.no_exec, tests_by_task, resmoke_cmd, options.report_file)


if __name__ == "__main__":
    main()
