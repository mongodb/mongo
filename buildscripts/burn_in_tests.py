#!/usr/bin/env python3
"""Command line utility for determining what jstests have been added or modified."""

import collections
import copy
import json
import optparse
import os.path
import subprocess
import shlex
import sys
import datetime
import logging

from math import ceil

import yaml
import requests

from shrub.config import Configuration
from shrub.command import CommandDefinition
from shrub.task import TaskDependency
from shrub.variant import DisplayTaskDefinition
from shrub.variant import TaskSpec
from shrub.operations import CmdTimeoutUpdate

from evergreen.api import RetryingEvergreenApi

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts import git
from buildscripts import resmokelib
from buildscripts.ciconfig import evergreen
from buildscripts.util import teststats
# pylint: enable=wrong-import-position

LOGGER = logging.getLogger(__name__)

AVG_TEST_RUNTIME_ANALYSIS_DAYS = 14
AVG_TEST_TIME_MULTIPLIER = 3
CONFIG_FILE = "../src/.evergreen.yml"
REPEAT_SUITES = 2
EVERGREEN_FILE = "etc/evergreen.yml"
MIN_AVG_TEST_OVERFLOW_SEC = 60
MIN_AVG_TEST_TIME_SEC = 5 * 60
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

    parser.add_option("--project", dest="project", default="mongodb-mongo-master",
                      help="The project the test history will be requested for.")

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


def find_last_activated_task(revisions, variant, project, evg_api):
    """
    Search the given list of revisions for the first build that was activated in evergreen.

    :param revisions: List of revisions to search.
    :param variant: Build variant to query for.
    :param project: Project being run against.
    :param evg_api: Evergreen api.
    :return: First revision from list that has been activated.
    """
    prefix = project.replace("-", "_")

    for githash in revisions:
        version_id = f"{prefix}_{githash}"
        version = evg_api.version_by_id(version_id)

        build = version.build_by_variant(variant)
        if build.activated:
            return githash

    return None


def find_changed_tests(  # pylint: disable=too-many-locals,too-many-arguments
        branch_name, base_commit, max_revisions, buildvariant, project, check_evergreen, evg_api):
    """
    Find the changed tests.

    Use git to find which files have changed in this patch.
    TODO: This should be expanded to search for enterprise modules.
    The returned file paths are in normalized form (see os.path.normpath(path)).

    :param branch_name: Branch being run against.
    :param base_commit: Commit changes are made on top of.
    :param max_revisions: Max number of revisions to search through.
    :param buildvariant: Build variant burn is being run on.
    :param project: Project that is being run on.
    :param check_evergreen: Should evergreen be checked for an activated build.
    :param evg_api: Evergreen api.
    :returns: List of changed tests.
    """

    changed_tests = []

    repo = git.Repository(".")

    if base_commit is None:
        base_commit = repo.get_merge_base([branch_name + "@{upstream}", "HEAD"])

    if check_evergreen:
        # We're going to check up to 200 commits in Evergreen for the last scheduled one.
        # The current commit will be activated in Evergreen; we use --skip to start at the
        # previous commit when trying to find the most recent preceding commit that has been
        # activated.
        revs_to_check = repo.git_rev_list([base_commit, "--max-count=200", "--skip=1"]).splitlines()
        last_activated = find_last_activated_task(revs_to_check, buildvariant, project, evg_api)
        if last_activated is None:
            # When the current commit is the first time 'buildvariant' has run, there won't be a
            # commit among 'revs_to_check' that's been activated in Evergreen. We handle this by
            # only considering tests changed in the current commit.
            last_activated = "HEAD"
        print("Comparing current branch against", last_activated)
        revisions = repo.git_rev_list([base_commit + "..." + last_activated]).splitlines()
        base_commit = last_activated
    else:
        revisions = repo.git_rev_list([base_commit + "...HEAD"]).splitlines()

    revision_count = len(revisions)
    if revision_count > max_revisions:
        print(("There are too many revisions included ({}). This is likely because your base"
               " branch is not {}. You can allow us to review more than {} revisions by using"
               " the --maxRevisions option.".format(revision_count, branch_name, max_revisions)))
        return changed_tests

    changed_files = repo.git_diff(["--name-only", base_commit]).splitlines()
    # New files ("untracked" in git terminology) won't show up in the git diff results.
    untracked_files = repo.git_status(["--porcelain"]).splitlines()

    # The lines with untracked files start with '?? '.
    for line in untracked_files:
        if line.startswith("?"):
            (_, line) = line.split(" ", 1)
            changed_files.append(line)

    for line in changed_files:
        line = line.rstrip()
        # Check that the file exists because it may have been moved or deleted in the patch.
        if os.path.splitext(line)[1] != ".js" or not os.path.isfile(line):
            continue
        if "jstests" in line:
            path = os.path.normpath(line)
            changed_tests.append(path)
    return changed_tests


def find_excludes(selector_file):
    """Parse etc/burn_in_tests.yml. Returns lists of excluded suites, tasks & tests."""

    if not selector_file:
        return ([], [], [])

    with open(selector_file, "r") as fstream:
        yml = yaml.safe_load(fstream)

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


def _parse_avg_test_runtime(test, task_avg_test_runtime_stats):
    """
    Parse list of teststats to find runtime for particular test.

    :param task_avg_test_runtime_stats: Teststat data.
    :param test: Test name.
    :return: Historical average runtime of the test.
    """
    for test_stat in task_avg_test_runtime_stats:
        if test_stat.test_name == test:
            return test_stat.runtime
    return None


def _calculate_timeout(avg_test_runtime):
    """
    Calculate timeout_secs for the Evergreen task.

    :param avg_test_runtime: How long a test has historically taken to run.
    :return: The test runtime times AVG_TEST_TIME_MULTIPLIER, or MIN_AVG_TEST_TIME_SEC (whichever
        is higher).
    """
    return max(MIN_AVG_TEST_TIME_SEC, ceil(avg_test_runtime * AVG_TEST_TIME_MULTIPLIER))


def _calculate_exec_timeout(options, avg_test_runtime):
    """
    Calculate exec_timeout_secs for the Evergreen task.

    :param avg_test_runtime: How long a test has historically taken to run.
    :return: repeat_tests_secs + an amount of padding time so that the test has time to finish on
        its final run.
    """
    test_execution_time_over_limit = avg_test_runtime - (
        options.repeat_tests_secs % avg_test_runtime)
    test_execution_time_over_limit = max(MIN_AVG_TEST_OVERFLOW_SEC, test_execution_time_over_limit)
    return ceil(options.repeat_tests_secs +
                (test_execution_time_over_limit * AVG_TEST_TIME_MULTIPLIER))


def _generate_timeouts(options, commands, test, task_avg_test_runtime_stats):
    """
    Add timeout.update command to list of commands for a burn in execution task.

    :param options: Command line options.
    :param commands: List of commands for a burn in execution task.
    :param test: Test name.
    :param task_avg_test_runtime_stats: Teststat data.
    """
    if task_avg_test_runtime_stats:
        avg_test_runtime = _parse_avg_test_runtime(test, task_avg_test_runtime_stats)
        if avg_test_runtime:
            cmd_timeout = CmdTimeoutUpdate()
            LOGGER.debug("Avg test runtime for test %s is: %s", test, avg_test_runtime)

            timeout = _calculate_timeout(avg_test_runtime)
            cmd_timeout.timeout(timeout)

            exec_timeout = _calculate_exec_timeout(options, avg_test_runtime)
            cmd_timeout.exec_timeout(exec_timeout)

            commands.append(cmd_timeout.validate().resolve())


def _get_task_runtime_history(evg_api, project, task, variant):
    """
    Fetch historical average runtime for all tests in a task from Evergreen API.

    :param evg_api: Evergreen API.
    :param project: Project name.
    :param task: Task name.
    :param variant: Variant name.
    :return: Test historical runtimes, parsed into teststat objects.
    """
    try:
        end_date = datetime.datetime.utcnow().replace(microsecond=0)
        start_date = end_date - datetime.timedelta(days=AVG_TEST_RUNTIME_ANALYSIS_DAYS)
        data = evg_api.test_stats_by_project(project, after_date=start_date.strftime("%Y-%m-%d"),
                                             before_date=end_date.strftime("%Y-%m-%d"),
                                             tasks=[task], variants=[variant], group_by="test",
                                             group_num_days=AVG_TEST_RUNTIME_ANALYSIS_DAYS)
        test_runtimes = teststats.TestStats(data).get_tests_runtimes()
        LOGGER.debug("Test_runtime data parsed from Evergreen history: %s", test_runtimes)
        return test_runtimes
    except requests.HTTPError as err:
        if err.response.status_code == requests.codes.SERVICE_UNAVAILABLE:
            # Evergreen may return a 503 when the service is degraded.
            # We fall back to returning no test history
            return []
        else:
            raise


def create_generate_tasks_config(evg_api, evg_config, options, tests_by_task, include_gen_task):
    """Create the config for the Evergreen generate.tasks file."""
    # pylint: disable=too-many-locals
    task_specs = []
    task_names = []
    if include_gen_task:
        task_names.append(BURN_IN_TESTS_GEN_TASK)
    for task in sorted(tests_by_task):
        multiversion_path = tests_by_task[task].get("use_multiversion")
        task_avg_test_runtime_stats = _get_task_runtime_history(evg_api, options.project, task,
                                                                options.buildvariant)
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
            _generate_timeouts(options, commands, test, task_avg_test_runtime_stats)
            commands.append(CommandDefinition().function("do setup"))
            if multiversion_path:
                run_tests_vars["task_path_suffix"] = multiversion_path
                commands.append(CommandDefinition().function("do multiversion setup"))
            commands.append(CommandDefinition().function("run tests").vars(run_tests_vars))
            evg_sub_task.commands(commands)

    display_task = DisplayTaskDefinition(BURN_IN_TESTS_TASK).execution_tasks(task_names)
    evg_config.variant(_get_run_buildvariant(options)).tasks(task_specs).display_task(display_task)
    return evg_config


def create_tests_by_task(options, evg_api):
    """
    Create a list of tests by task.

    :param options: Options.
    :param evg_api: Evergreen api.
    :return: Tests by task
    """
    # Parse the Evergreen project configuration file.
    evergreen_conf = evergreen.parse_evergreen_file(EVERGREEN_FILE)

    changed_tests = find_changed_tests(options.branch, options.base_commit, options.max_revisions,
                                       options.buildvariant, options.project,
                                       options.check_evergreen, evg_api)
    exclude_suites, exclude_tasks, exclude_tests = find_excludes(SELECTOR_FILE)
    changed_tests = filter_tests(changed_tests, exclude_tests)

    if changed_tests:
        suites = resmokelib.suitesconfig.get_suites(suite_files=SUITE_FILES,
                                                    test_files=changed_tests)
        tests_by_executor = create_executor_list(suites, exclude_suites)
        tests_by_task = create_task_list(evergreen_conf, options.buildvariant, tests_by_executor,
                                         exclude_tasks)
    else:
        print("No new or modified tests found.")
        tests_by_task = {}

    return tests_by_task


def create_generate_tasks_file(evg_api, options, tests_by_task):
    """Create the Evergreen generate.tasks file."""

    evg_config = Configuration()
    evg_config = create_generate_tasks_config(evg_api, evg_config, options, tests_by_task,
                                              include_gen_task=True)
    _write_json_file(evg_config.to_map(), options.generate_tasks_file)


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


def main(evg_api):
    """Execute Main program."""

    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=logging.DEBUG,
        stream=sys.stdout,
    )

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
        tests_by_task = create_tests_by_task(options, evg_api)

        if options.test_list_outfile:
            _write_json_file(tests_by_task, options.test_list_outfile)

    if options.generate_tasks_file:
        create_generate_tasks_file(evg_api, options, tests_by_task)
    else:
        run_tests(options.no_exec, tests_by_task, resmoke_cmd, options.report_file)


if __name__ == "__main__":
    main(RetryingEvergreenApi.get_api(config_file=CONFIG_FILE))
