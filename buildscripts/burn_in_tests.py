#!/usr/bin/env python3
"""Command line utility for determining what jstests have been added or modified."""

import copy
import datetime
import json
import logging
import os.path
import shlex
import subprocess
import sys

from math import ceil
from collections import defaultdict
from typing import Optional, Set, Tuple, List, Dict

import click
import requests
import structlog
from structlog.stdlib import LoggerFactory
import yaml

from git import Repo
from evergreen.api import RetryingEvergreenApi, EvergreenApi
from shrub.config import Configuration

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.patch_builds.change_data import find_changed_files
from buildscripts.resmokelib.suitesconfig import create_test_membership_map, get_suites

from buildscripts.resmokelib.utils import default_if_none, globstar
from buildscripts.ciconfig.evergreen import parse_evergreen_file, ResmokeArgs, \
    EvergreenProjectConfig, VariantTask
from buildscripts.util.teststats import TestStats
from buildscripts.util.taskname import name_generated_task
from buildscripts.patch_builds.task_generation import resmoke_commands, TimeoutInfo, TaskList

# pylint: enable=wrong-import-position

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.getLogger(__name__)
EXTERNAL_LOGGERS = {
    "evergreen",
    "git",
    "urllib3",
}

AVG_TEST_RUNTIME_ANALYSIS_DAYS = 14
AVG_TEST_SETUP_SEC = 4 * 60
AVG_TEST_TIME_MULTIPLIER = 3
CONFIG_FILE = ".evergreen.yml"
DEFAULT_PROJECT = "mongodb-mongo-master"
REPEAT_SUITES = 2
EVERGREEN_FILE = "etc/evergreen.yml"
MAX_TASKS_TO_CREATE = 1000
MIN_AVG_TEST_OVERFLOW_SEC = float(60)
MIN_AVG_TEST_TIME_SEC = 5 * 60
# The executor_file and suite_files defaults are required to make the suite resolver work
# correctly.
SELECTOR_FILE = "etc/burn_in_tests.yml"
SUITE_FILES = ["with_server"]

SUPPORTED_TEST_KINDS = ("fsm_workload_test", "js_test", "json_schema_test",
                        "multi_stmt_txn_passthrough", "parallel_fsm_workload_test")

BURN_IN_TESTS_GEN_TASK = "burn_in_tests_gen"
BURN_IN_TESTS_TASK = "burn_in_tests"


class RepeatConfig(object):
    """Configuration for how tests should be repeated."""

    def __init__(self, repeat_tests_secs: Optional[int] = None,
                 repeat_tests_min: Optional[int] = None, repeat_tests_max: Optional[int] = None,
                 repeat_tests_num: Optional[int] = None):
        """
        Create a Repeat Config.

        :param repeat_tests_secs: Repeat test for this number of seconds.
        :param repeat_tests_min: Repeat the test at least this many times.
        :param repeat_tests_max: At most repeat the test this many times.
        :param repeat_tests_num: Repeat the test exactly this many times.
        """
        self.repeat_tests_secs = repeat_tests_secs
        self.repeat_tests_min = repeat_tests_min
        self.repeat_tests_max = repeat_tests_max
        self.repeat_tests_num = repeat_tests_num

    def validate(self):
        """
        Raise an exception if this configuration is invalid.

        :return: self.
        """
        if self.repeat_tests_num and self.repeat_tests_secs:
            raise ValueError("Cannot specify --repeat-tests and --repeat-tests-secs")

        if self.repeat_tests_max:
            if not self.repeat_tests_secs:
                raise ValueError("Must specify --repeat-tests-secs with --repeat-tests-max")

            if self.repeat_tests_min and self.repeat_tests_min > self.repeat_tests_max:
                raise ValueError("--repeat-tests-secs-min is greater than --repeat-tests-max")

        if self.repeat_tests_min and not self.repeat_tests_secs:
            raise ValueError("Must specify --repeat-tests-secs with --repeat-tests-min")
        return self

    def generate_resmoke_options(self) -> str:
        """
        Generate the resmoke options to repeat a test.

        :return: Resmoke options to repeat a test.
        """
        if self.repeat_tests_secs:
            repeat_options = f" --repeatTestsSecs={self.repeat_tests_secs} "
            if self.repeat_tests_min:
                repeat_options += f" --repeatTestsMin={self.repeat_tests_min} "
            if self.repeat_tests_max:
                repeat_options += f" --repeatTestsMax={self.repeat_tests_max} "
            return repeat_options

        repeat_suites = self.repeat_tests_num if self.repeat_tests_num else REPEAT_SUITES
        return f" --repeatSuites={repeat_suites} "

    def __repr__(self):
        """Build string representation of object for debugging."""
        return "".join([
            f"RepeatConfig[num={self.repeat_tests_num}, secs={self.repeat_tests_secs}, ",
            f"min={self.repeat_tests_min}, max={self.repeat_tests_max}]",
        ])


class GenerateConfig(object):
    """Configuration for how to generate tasks."""

    def __init__(self, build_variant: str, project: str, run_build_variant: Optional[str] = None,
                 distro: Optional[str] = None):
        # pylint: disable=too-many-arguments,too-many-locals
        """
        Create a GenerateConfig.

        :param build_variant: Build variant to get tasks from.
        :param project: Project to run tasks on.
        :param run_build_variant: Build variant to run new tasks on.
        :param distro: Distro to run tasks on.
        """
        self.build_variant = build_variant
        self._run_build_variant = run_build_variant
        self.distro = distro
        self.project = project

    @property
    def run_build_variant(self):
        """Build variant tasks should run against."""
        if self._run_build_variant:
            return self._run_build_variant
        return self.build_variant

    def validate(self, evg_conf: EvergreenProjectConfig):
        """
        Raise an exception if this configuration is invalid.

        :param evg_conf: Evergreen configuration.
        :return: self.
        """
        self._check_variant(self.build_variant, evg_conf)
        return self

    @staticmethod
    def _check_variant(build_variant: str, evg_conf: EvergreenProjectConfig):
        """
        Check if the build_variant is found in the evergreen file.

        :param build_variant: Build variant to check.
        :param evg_conf: Evergreen configuration to check against.
        """
        if not evg_conf.get_variant(build_variant):
            raise ValueError(f"Build variant '{build_variant}' not found in Evergreen file")


def _is_file_a_test_file(file_path: str) -> bool:
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


def find_changed_tests(repo: Repo) -> Set[str]:
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


def find_excludes(selector_file: str) -> Tuple[List, List, List]:
    """Parse etc/burn_in_tests.yml. Returns lists of excluded suites, tasks & tests."""

    if not selector_file:
        return [], [], []

    LOGGER.debug("reading configuration", config_file=selector_file)
    with open(selector_file, "r") as fstream:
        yml = yaml.safe_load(fstream)

    try:
        js_test = yml["selector"]["js_test"]
    except KeyError:
        raise Exception(f"The selector file {selector_file} is missing the 'selector.js_test' key")

    return (default_if_none(js_test.get("exclude_suites"), []),
            default_if_none(js_test.get("exclude_tasks"), []),
            default_if_none(js_test.get("exclude_tests"), []))


def filter_tests(tests: Set[str], exclude_tests: [str]) -> Set[str]:
    """
    Exclude tests which have been blacklisted.

    :param tests: Set of tests to filter.
    :param exclude_tests: Tests to filter out.
    :return: Set of tests with exclude_tests filtered out.
    """
    if not exclude_tests or not tests:
        return tests

    # The exclude_tests can be specified using * and ** to specify directory and file patterns.
    excluded_globbed = set()
    for exclude_test_pattern in exclude_tests:
        excluded_globbed.update(globstar.iglob(exclude_test_pattern))

    LOGGER.debug("Excluding test pattern", excluded=excluded_globbed)
    return tests - excluded_globbed


def create_executor_list(suites, exclude_suites):
    """Create the executor list.

    Looks up what other resmoke suites run the tests specified in the suites
    parameter. Returns a dict keyed by suite name / executor, value is tests
    to run under that executor.
    """
    test_membership = create_test_membership_map(test_kind=SUPPORTED_TEST_KINDS)

    memberships = defaultdict(list)
    for suite in suites:
        LOGGER.debug("Adding tests for suite", suite=suite, tests=suite.tests)
        for test in suite.tests:
            LOGGER.debug("membership for test", test=test, membership=test_membership[test])
            for executor in set(test_membership[test]) - set(exclude_suites):
                if test not in memberships[executor]:
                    memberships[executor].append(test)
    return memberships


def _get_task_name(task):
    """
    Return the task var from a "generate resmoke task" instead of the task name.

    :param task: task to get name of.
    """

    if task.is_generate_resmoke_task:
        return task.generated_task_name

    return task.name


def _set_resmoke_args(task):
    """
    Set the resmoke args to include the --suites option.

    The suite name from "generate resmoke tasks" can be specified as a var or directly in the
    resmoke_args.
    """

    resmoke_args = task.resmoke_args
    suite_name = ResmokeArgs.get_arg(resmoke_args, "suites")
    if task.is_generate_resmoke_task:
        suite_name = task.get_vars_suite_name(task.generate_resmoke_tasks_command["vars"])

    return ResmokeArgs.get_updated_arg(resmoke_args, "suites", suite_name)


def _distro_to_run_task_on(task: VariantTask, evg_proj_config: EvergreenProjectConfig,
                           build_variant: str) -> str:
    """
    Determine what distro an task should be run on.

    For normal tasks, the distro will be the default for the build variant unless the task spec
    specifies a particular distro to run on.

    For generated tasks, the distro will be the default for the build variant unless (1) the
    "use_large_distro" flag is set as a "var" in the "generate resmoke tasks" command of the
    task definition and (2) the build variant defines the "large_distro_name" in its expansions.

    :param task: Task being run.
    :param evg_proj_config: Evergreen project configuration.
    :param build_variant: Build Variant task is being run on.
    :return: Distro task should be run on.
    """
    task_def = evg_proj_config.get_task(task.name)
    if task_def.is_generate_resmoke_task:
        resmoke_vars = task_def.generate_resmoke_tasks_command["vars"]
        if "use_large_distro" in resmoke_vars:
            bv = evg_proj_config.get_variant(build_variant)
            if "large_distro_name" in bv.raw["expansions"]:
                return bv.raw["expansions"]["large_distro_name"]

    return task.run_on[0]


def _gather_task_info(task: VariantTask, tests_by_suite: Dict,
                      evg_proj_config: EvergreenProjectConfig, build_variant: str) -> Dict:
    """
    Gather the information needed to run the given task.

    :param task: Task to be run.
    :param tests_by_suite: Dict of suites.
    :param evg_proj_config: Evergreen project configuration.
    :param build_variant: Build variant task will be run on.
    :return: Dictionary of information needed to run task.
    """
    return {
        "resmoke_args": _set_resmoke_args(task),
        "tests": tests_by_suite[task.resmoke_suite],
        "use_multiversion": task.multiversion_path,
        "distro": _distro_to_run_task_on(task, evg_proj_config, build_variant)
    }  # yapf: disable


def create_task_list(evergreen_conf: EvergreenProjectConfig, build_variant: str,
                     tests_by_suite: Dict[str, List[str]], exclude_tasks: [str]):
    """
    Find associated tasks for the specified build_variant and suites.

    Returns a dict keyed by task_name, with executor, resmoke_args & tests, i.e.,
    {'jsCore_small_oplog':
        {'resmoke_args': '--suites=core_small_oplog --storageEngine=inMemory',
         'tests': ['jstests/core/all2.js', 'jstests/core/all3.js'],
         'use_multiversion': '/data/multiversion'}
    }

    :param evergreen_conf: Evergreen configuration for project.
    :param build_variant: Build variant to select tasks from.
    :param tests_by_suite: Suites to be run.
    :param exclude_tasks: Tasks to exclude.
    :return: Dict of tasks to run with run configuration.
    """
    log = LOGGER.bind(build_variant=build_variant)

    log.debug("creating task list for suites", suites=tests_by_suite, exclude_tasks=exclude_tasks)
    evg_build_variant = evergreen_conf.get_variant(build_variant)
    if not evg_build_variant:
        log.warning("Buildvariant not found in evergreen config")
        raise ValueError(f"Buildvariant ({build_variant} not found in evergreen configuration")

    # Find all the build variant tasks.
    exclude_tasks_set = set(exclude_tasks)
    all_variant_tasks = {
        _get_task_name(task): task
        for task in evg_build_variant.tasks
        if task.name not in exclude_tasks_set and task.combined_resmoke_args
    }

    # Return the list of tasks to run for the specified suite.
    task_list = {
        task_name: _gather_task_info(task, tests_by_suite, evergreen_conf, build_variant)
        for task_name, task in all_variant_tasks.items() if task.resmoke_suite in tests_by_suite
    }

    log.debug("Found task list", task_list=task_list)
    return task_list


def _write_json_file(json_data, pathname):
    """Write out a JSON file."""

    with open(pathname, "w") as fstream:
        json.dump(json_data, fstream, indent=4)


def _set_resmoke_cmd(repeat_config: RepeatConfig, resmoke_args: [str]) -> [str]:
    """Build the resmoke command, if a resmoke.py command wasn't passed in."""
    new_args = [sys.executable, "buildscripts/resmoke.py", "run"]
    if resmoke_args:
        new_args = copy.deepcopy(resmoke_args)

    new_args += repeat_config.generate_resmoke_options().split()
    LOGGER.debug("set resmoke command", new_args=new_args)
    return new_args


def _parse_avg_test_runtime(test: str, task_avg_test_runtime_stats: [TestStats]) -> Optional[float]:
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


def _calculate_timeout(avg_test_runtime: float) -> int:
    """
    Calculate timeout_secs for the Evergreen task.

    :param avg_test_runtime: How long a test has historically taken to run.
    :return: The test runtime times AVG_TEST_TIME_MULTIPLIER, or MIN_AVG_TEST_TIME_SEC (whichever
        is higher).
    """
    return max(MIN_AVG_TEST_TIME_SEC, ceil(avg_test_runtime * AVG_TEST_TIME_MULTIPLIER))


def _calculate_exec_timeout(repeat_config: RepeatConfig, avg_test_runtime: float) -> int:
    """
    Calculate exec_timeout_secs for the Evergreen task.

    :param repeat_config: Information about how the test will repeat.
    :param avg_test_runtime: How long a test has historically taken to run.
    :return: repeat_tests_secs + an amount of padding time so that the test has time to finish on
        its final run.
    """
    LOGGER.debug("Calculating exec timeout", repeat_config=repeat_config,
                 avg_test_runtime=avg_test_runtime)
    repeat_tests_secs = repeat_config.repeat_tests_secs
    if avg_test_runtime > repeat_tests_secs and repeat_config.repeat_tests_min:
        # If a single execution of the test takes longer than the repeat time, then we don't
        # have to worry about the repeat time at all and can just use the average test runtime
        # and minimum number of executions to calculate the exec timeout value.
        return ceil(avg_test_runtime * AVG_TEST_TIME_MULTIPLIER * repeat_config.repeat_tests_min)

    test_execution_time_over_limit = avg_test_runtime - (repeat_tests_secs % avg_test_runtime)
    test_execution_time_over_limit = max(MIN_AVG_TEST_OVERFLOW_SEC, test_execution_time_over_limit)
    return ceil(repeat_tests_secs + (test_execution_time_over_limit * AVG_TEST_TIME_MULTIPLIER) +
                AVG_TEST_SETUP_SEC)


def _generate_timeouts(repeat_config: RepeatConfig, test: str,
                       task_avg_test_runtime_stats: [TestStats]) -> TimeoutInfo:
    """
    Add timeout.update command to list of commands for a burn in execution task.

    :param repeat_config: Information on how the test will repeat.
    :param test: Test name.
    :param task_avg_test_runtime_stats: Teststat data.
    :return: TimeoutInfo to use.
    """
    if task_avg_test_runtime_stats:
        avg_test_runtime = _parse_avg_test_runtime(test, task_avg_test_runtime_stats)
        if avg_test_runtime:
            LOGGER.debug("Avg test runtime", test=test, runtime=avg_test_runtime)

            timeout = _calculate_timeout(avg_test_runtime)
            exec_timeout = _calculate_exec_timeout(repeat_config, avg_test_runtime)
            LOGGER.debug("Using timeout overrides", exec_timeout=exec_timeout, timeout=timeout)
            timeout_info = TimeoutInfo.overridden(exec_timeout, timeout)

            LOGGER.debug("Override runtime for test", test=test, timeout=timeout_info)
            return timeout_info

    return TimeoutInfo.default_timeout()


def _get_task_runtime_history(evg_api: Optional[EvergreenApi], project: str, task: str,
                              variant: str):
    """
    Fetch historical average runtime for all tests in a task from Evergreen API.

    :param evg_api: Evergreen API.
    :param project: Project name.
    :param task: Task name.
    :param variant: Variant name.
    :return: Test historical runtimes, parsed into teststat objects.
    """
    if not evg_api:
        return []

    try:
        end_date = datetime.datetime.utcnow().replace(microsecond=0)
        start_date = end_date - datetime.timedelta(days=AVG_TEST_RUNTIME_ANALYSIS_DAYS)
        data = evg_api.test_stats_by_project(project, after_date=start_date.strftime("%Y-%m-%d"),
                                             before_date=end_date.strftime("%Y-%m-%d"),
                                             tasks=[task], variants=[variant], group_by="test",
                                             group_num_days=AVG_TEST_RUNTIME_ANALYSIS_DAYS)
        test_runtimes = TestStats(data).get_tests_runtimes()
        return test_runtimes
    except requests.HTTPError as err:
        if err.response.status_code == requests.codes.SERVICE_UNAVAILABLE:
            # Evergreen may return a 503 when the service is degraded.
            # We fall back to returning no test history
            return []
        else:
            raise


def create_generate_tasks_config(evg_config: Configuration, tests_by_task: Dict,
                                 generate_config: GenerateConfig, repeat_config: RepeatConfig,
                                 evg_api: Optional[EvergreenApi], include_gen_task: bool = True,
                                 task_prefix: str = "burn_in") -> Configuration:
    # pylint: disable=too-many-arguments,too-many-locals
    """
    Create the config for the Evergreen generate.tasks file.

    :param evg_config: Shrub configuration to add to.
    :param tests_by_task: Dictionary of tests to generate tasks for.
    :param generate_config: Configuration of what to generate.
    :param repeat_config: Configuration of how to repeat tests.
    :param evg_api: Evergreen API.
    :param include_gen_task: Should generating task be include in display task.
    :param task_prefix: Prefix all task names with this.
    :return: Shrub configuration with added tasks.
    """
    task_list = TaskList(evg_config)
    resmoke_options = repeat_config.generate_resmoke_options()
    for task in sorted(tests_by_task):
        multiversion_path = tests_by_task[task].get("use_multiversion")
        task_runtime_stats = _get_task_runtime_history(evg_api, generate_config.project, task,
                                                       generate_config.build_variant)
        resmoke_args = tests_by_task[task]["resmoke_args"]
        test_list = tests_by_task[task]["tests"]
        distro = tests_by_task[task].get("distro", generate_config.distro)
        for index, test in enumerate(test_list):
            # Evergreen always uses a unix shell, even on Windows, so instead of using os.path.join
            # here, just use the forward slash; otherwise the path separator will be treated as
            # the escape character on Windows.
            sub_task_name = name_generated_task(f"{task_prefix}:{task}", index, len(test_list),
                                                generate_config.run_build_variant)
            LOGGER.debug("Generating sub-task", sub_task=sub_task_name)

            test_unix_style = test.replace('\\', '/')
            run_tests_vars = {"resmoke_args": f"{resmoke_args} {resmoke_options} {test_unix_style}"}
            if multiversion_path:
                run_tests_vars["task_path_suffix"] = multiversion_path
            timeout = _generate_timeouts(repeat_config, test, task_runtime_stats)
            commands = resmoke_commands("run tests", run_tests_vars, timeout, multiversion_path)

            task_list.add_task(sub_task_name, commands, ["compile"], distro)

    existing_tasks = [BURN_IN_TESTS_GEN_TASK] if include_gen_task else None
    task_list.add_to_variant(generate_config.run_build_variant, BURN_IN_TESTS_TASK, existing_tasks)
    return evg_config


def create_task_list_for_tests(
        changed_tests: Set[str], build_variant: str, evg_conf: EvergreenProjectConfig,
        exclude_suites: Optional[List] = None, exclude_tasks: Optional[List] = None) -> Dict:
    """
    Create a list of tests by task for the given tests.

    :param changed_tests: Set of test that have changed.
    :param build_variant: Build variant to collect tasks from.
    :param evg_conf: Evergreen configuration.
    :param exclude_suites: Suites to exclude.
    :param exclude_tasks: Tasks to exclude.
    :return: Tests by task.
    """
    if not exclude_suites:
        exclude_suites = []
    if not exclude_tasks:
        exclude_tasks = []

    suites = get_suites(suite_files=SUITE_FILES, test_files=changed_tests)
    LOGGER.debug("Found suites to run", suites=suites)

    tests_by_executor = create_executor_list(suites, exclude_suites)
    LOGGER.debug("tests_by_executor", tests_by_executor=tests_by_executor)

    return create_task_list(evg_conf, build_variant, tests_by_executor, exclude_tasks)


def create_tests_by_task(build_variant: str, repo: Repo, evg_conf: EvergreenProjectConfig) -> Dict:
    """
    Create a list of tests by task.

    :param build_variant: Build variant to collect tasks from.
    :param repo: Git repo being tracked.
    :param evg_conf: Evergreen configuration.
    :return: Tests by task.
    """
    changed_tests = find_changed_tests(repo)
    exclude_suites, exclude_tasks, exclude_tests = find_excludes(SELECTOR_FILE)
    changed_tests = filter_tests(changed_tests, exclude_tests)

    if changed_tests:
        return create_task_list_for_tests(changed_tests, build_variant, evg_conf, exclude_suites,
                                          exclude_tasks)

    LOGGER.info("No new or modified tests found.")
    return {}


# pylint: disable=too-many-arguments
def create_generate_tasks_file(tests_by_task: Dict, generate_config: GenerateConfig,
                               repeat_config: RepeatConfig, evg_api: Optional[EvergreenApi],
                               task_prefix: str = 'burn_in', include_gen_task: bool = True) -> Dict:
    """
    Create an Evergreen generate.tasks file to run the given tasks and tests.

    :param tests_by_task: Dictionary of tests and tasks to run.
    :param generate_config: Information about how burn_in should generate tasks.
    :param repeat_config: Information about how burn_in should repeat tests.
    :param evg_api: Evergreen api.
    :param task_prefix: Prefix to start generated task's name with.
    :param include_gen_task: Should the generating task be included in the display task.
    :returns: Configuration to pass to 'generate.tasks'.
    """
    evg_config = Configuration()
    evg_config = create_generate_tasks_config(
        evg_config, tests_by_task, generate_config, repeat_config, evg_api,
        include_gen_task=include_gen_task, task_prefix=task_prefix)

    json_config = evg_config.to_map()
    tasks_to_create = len(json_config.get('tasks', []))
    if tasks_to_create > MAX_TASKS_TO_CREATE:
        LOGGER.warning("Attempting to create more tasks than max, aborting", tasks=tasks_to_create,
                       max=MAX_TASKS_TO_CREATE)
        sys.exit(1)
    return json_config


def run_tests(tests_by_task: Dict, resmoke_cmd: [str]):
    """
    Run the given tests locally.

    This function will exit with a non-zero return code on test failure.

    :param tests_by_task: Dictionary of tests to run.
    :param resmoke_cmd: Parameter to use when calling resmoke.
    """
    for task in sorted(tests_by_task):
        log = LOGGER.bind(task=task)
        new_resmoke_cmd = copy.deepcopy(resmoke_cmd)
        new_resmoke_cmd.extend(shlex.split(tests_by_task[task]["resmoke_args"]))
        new_resmoke_cmd.extend(tests_by_task[task]["tests"])
        log.debug("starting execution of task")
        try:
            subprocess.check_call(new_resmoke_cmd, shell=False)
        except subprocess.CalledProcessError as err:
            log.warning("Resmoke returned an error with task", error=err.returncode)
            sys.exit(err.returncode)


def _configure_logging(verbose: bool):
    """
    Configure logging for the application.

    :param verbose: If True set log level to DEBUG.
    """
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=level,
        stream=sys.stdout,
    )
    for log_name in EXTERNAL_LOGGERS:
        logging.getLogger(log_name).setLevel(logging.WARNING)


def _get_evg_api(evg_api_config: str, local_mode: bool) -> Optional[EvergreenApi]:
    """
    Get an instance of the Evergreen Api.

    :param evg_api_config: Config file with evg auth information.
    :param local_mode: If true, do not connect to Evergreen API.
    :return: Evergreen Api instance.
    """
    if not local_mode:
        return RetryingEvergreenApi.get_api(config_file=evg_api_config)
    return None


def burn_in(repeat_config: RepeatConfig, generate_config: GenerateConfig, resmoke_args: str,
            generate_tasks_file: str, no_exec: bool, evg_conf: EvergreenProjectConfig, repo: Repo,
            evg_api: EvergreenApi):
    """
    Run burn_in_tests with the given configuration.

    :param repeat_config: Config on how much to repeat tests.
    :param generate_config: Config on how to generate tests.
    :param resmoke_args: Arguments to pass to resmoke.
    :param generate_tasks_file: File to write generated config to.
    :param no_exec: Do not execute tests, just discover tests to run.
    :param evg_conf: Evergreen configuration.
    :param repo: Git repository.
    :param evg_api: Evergreen API client.
    """
    # Populate the config values in order to use the helpers from resmokelib.suitesconfig.
    resmoke_cmd = _set_resmoke_cmd(repeat_config, list(resmoke_args))

    tests_by_task = create_tests_by_task(generate_config.build_variant, repo, evg_conf)
    LOGGER.debug("tests and tasks found", tests_by_task=tests_by_task)

    if generate_tasks_file:
        json_config = create_generate_tasks_file(tests_by_task, generate_config, repeat_config,
                                                 evg_api)
        _write_json_file(json_config, generate_tasks_file)
    elif not no_exec:
        run_tests(tests_by_task, resmoke_cmd)
    else:
        LOGGER.info("Not running tests due to 'no_exec' option.")


@click.command()
@click.option("--no-exec", "no_exec", default=False, is_flag=True,
              help="Do not execute the found tests.")
@click.option("--generate-tasks-file", "generate_tasks_file", default=None, metavar='FILE',
              help="Run in 'generate.tasks' mode. Store task config to given file.")
@click.option("--build-variant", "build_variant", default=None, metavar='BUILD_VARIANT',
              help="Tasks to run will be selected from this build variant.")
@click.option("--run-build-variant", "run_build_variant", default=None, metavar='BUILD_VARIANT',
              help="Burn in tasks will be generated on this build variant.")
@click.option("--distro", "distro", default=None, metavar='DISTRO',
              help="The distro the tasks will execute on.")
@click.option("--project", "project", default=DEFAULT_PROJECT, metavar='PROJECT',
              help="The evergreen project the tasks will execute on.")
@click.option("--repeat-tests", "repeat_tests_num", default=None, type=int,
              help="Number of times to repeat tests.")
@click.option("--repeat-tests-min", "repeat_tests_min", default=None, type=int,
              help="The minimum number of times to repeat tests if time option is specified.")
@click.option("--repeat-tests-max", "repeat_tests_max", default=None, type=int,
              help="The maximum number of times to repeat tests if time option is specified.")
@click.option("--repeat-tests-secs", "repeat_tests_secs", default=None, type=int, metavar="SECONDS",
              help="Repeat tests for the given time (in secs).")
@click.option("--evg-api-config", "evg_api_config", default=CONFIG_FILE, metavar="FILE",
              help="Configuration file with connection info for Evergreen API.")
@click.option("--local", "local_mode", default=False, is_flag=True,
              help="Local mode. Do not call out to evergreen api.")
@click.option("--verbose", "verbose", default=False, is_flag=True, help="Enable extra logging.")
@click.argument("resmoke_args", nargs=-1, type=click.UNPROCESSED)
# pylint: disable=too-many-arguments,too-many-locals
def main(build_variant, run_build_variant, distro, project, generate_tasks_file, no_exec,
         repeat_tests_num, repeat_tests_min, repeat_tests_max, repeat_tests_secs, resmoke_args,
         local_mode, evg_api_config, verbose):
    """
    Run new or changed tests in repeated mode to validate their stability.

    burn_in_tests detects jstests that are new or changed since the last git command and then
    runs those tests in a loop to validate their reliability.

    The `--repeat-*` arguments allow configuration of how burn_in_tests repeats tests. Tests can
    either be repeated a specified number of times with the `--repeat-tests` option, or they can
    be repeated for a certain time period with the `--repeat-tests-secs` option.

    There are two modes that burn_in_tests can run in:

    (1) Normal mode: by default burn_in_tests will attempt to run all detected tests the
    configured number of times. This is useful if you have a test or tests you would like to
    check before submitting a patch to evergreen.

    (2) By specifying the `--generate-tasks-file`, burn_in_tests will run generate a configuration
    file that can then be sent to the Evergreen 'generate.tasks' command to create evergreen tasks
    to do all the test executions. This is the mode used to run tests in patch builds.

    NOTE: There is currently a limit of the number of tasks burn_in_tests will attempt to generate
    in evergreen. The limit is 1000. If you change enough tests that more than 1000 tasks would
    be generated, burn_in_test will fail. This is to avoid generating more tasks than evergreen
    can handle.
    \f

    :param build_variant: Build variant to query tasks from.
    :param run_build_variant:Build variant to actually run against.
    :param distro: Distro to run tests on.
    :param project: Project to run tests on.
    :param generate_tasks_file: Create a generate tasks configuration in this file.
    :param no_exec: Just perform test discover, do not execute the tests.
    :param repeat_tests_num: Repeat each test this number of times.
    :param repeat_tests_min: Repeat each test at least this number of times.
    :param repeat_tests_max: Once this number of repetitions has been reached, stop repeating.
    :param repeat_tests_secs: Continue repeating tests for this number of seconds.
    :param resmoke_args: Arguments to pass through to resmoke.
    :param local_mode: Don't call out to the evergreen API (used for testing).
    :param evg_api_config: Location of configuration file to connect to evergreen.
    :param verbose: Log extra debug information.
    """
    _configure_logging(verbose)

    evg_conf = parse_evergreen_file(EVERGREEN_FILE)
    repeat_config = RepeatConfig(repeat_tests_secs=repeat_tests_secs,
                                 repeat_tests_min=repeat_tests_min,
                                 repeat_tests_max=repeat_tests_max,
                                 repeat_tests_num=repeat_tests_num)  # yapf: disable
    repeat_config.validate()
    generate_config = GenerateConfig(build_variant=build_variant,
                                     run_build_variant=run_build_variant,
                                     distro=distro,
                                     project=project)  # yapf: disable
    generate_config.validate(evg_conf)

    evg_api = _get_evg_api(evg_api_config, local_mode)
    repo = Repo(".")

    burn_in(repeat_config, generate_config, resmoke_args, generate_tasks_file, no_exec, evg_conf,
            repo, evg_api)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
