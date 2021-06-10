#!/usr/bin/env python3
"""Wrapper around burn_in_tests for evergreen execution."""
import logging
import os
import sys
from datetime import datetime, timedelta
from math import ceil
from typing import Optional, List, Dict, Set

import click
import requests
import structlog
from git import Repo
from shrub.v2 import ShrubProject, BuildVariant, Task, TaskDependency, ExistingTask
from evergreen import RetryingEvergreenApi, EvergreenApi

from buildscripts.burn_in_tests import RepeatConfig, BurnInExecutor, TaskInfo, FileChangeDetector, \
    DEFAULT_REPO_LOCATIONS, BurnInOrchestrator
from buildscripts.ciconfig.evergreen import parse_evergreen_file, EvergreenProjectConfig
from buildscripts.patch_builds.change_data import RevisionMap
from buildscripts.patch_builds.evg_change_data import generate_revision_map_from_manifest
from buildscripts.patch_builds.task_generation import TimeoutInfo, resmoke_commands, \
    validate_task_generation_limit
from buildscripts.util.fileops import write_file
from buildscripts.util.taskname import name_generated_task
from buildscripts.util.teststats import TestRuntime, HistoricTaskData

CONFIG_FILE = ".evergreen.yml"
DEFAULT_PROJECT = "mongodb-mongo-master"
DEFAULT_VARIANT = "enterprise-rhel-80-64-bit-dynamic-required"
EVERGREEN_FILE = "etc/evergreen.yml"
BURN_IN_TESTS_GEN_TASK = "burn_in_tests_gen"
BURN_IN_TESTS_TASK = "burn_in_tests"
TASK_WITH_ARTIFACTS = "archive_dist_test_debug"
AVG_TEST_RUNTIME_ANALYSIS_DAYS = 14
AVG_TEST_SETUP_SEC = 4 * 60
AVG_TEST_TIME_MULTIPLIER = 3
MIN_AVG_TEST_OVERFLOW_SEC = float(60)
MIN_AVG_TEST_TIME_SEC = 5 * 60

LOGGER = structlog.getLogger(__name__)
EXTERNAL_LOGGERS = {
    "evergreen",
    "git",
    "urllib3",
}


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


class GenerateConfig(object):
    """Configuration for how to generate tasks."""

    def __init__(self, build_variant: str, project: str, run_build_variant: Optional[str] = None,
                 distro: Optional[str] = None, task_id: Optional[str] = None,
                 task_prefix: str = "burn_in", include_gen_task: bool = True) -> None:
        # pylint: disable=too-many-arguments,too-many-locals
        """
        Create a GenerateConfig.

        :param build_variant: Build variant to get tasks from.
        :param project: Project to run tasks on.
        :param run_build_variant: Build variant to run new tasks on.
        :param distro: Distro to run tasks on.
        :param task_id: Evergreen task being run under.
        :param task_prefix: Prefix to include in generated task names.
        :param include_gen_task: Indicates the "_gen" task should be grouped in the display task.
        """
        self.build_variant = build_variant
        self._run_build_variant = run_build_variant
        self.distro = distro
        self.project = project
        self.task_id = task_id
        self.task_prefix = task_prefix
        self.include_gen_task = include_gen_task

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


def _parse_avg_test_runtime(test: str,
                            task_avg_test_runtime_stats: List[TestRuntime]) -> Optional[float]:
    """
    Parse list of test runtimes to find runtime for particular test.

    :param task_avg_test_runtime_stats: List of average historic runtimes of tests.
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


class TaskGenerator:
    """Class to generate task configurations."""

    def __init__(self, generate_config: GenerateConfig, repeat_config: RepeatConfig,
                 task_info: TaskInfo, task_runtime_stats: List[TestRuntime]) -> None:
        """
        Create a new task generator.

        :param generate_config: Generate configuration to use.
        :param repeat_config: Repeat configuration to use.
        :param task_info: Information about how tasks should be generated.
        :param task_runtime_stats: Historic runtime of tests associated with task.
        """
        self.generate_config = generate_config
        self.repeat_config = repeat_config
        self.task_info = task_info
        self.task_runtime_stats = task_runtime_stats

    def generate_timeouts(self, test: str) -> TimeoutInfo:
        """
        Add timeout.update command to list of commands for a burn in execution task.

        :param test: Test name.
        :return: TimeoutInfo to use.
        """
        if self.task_runtime_stats:
            avg_test_runtime = _parse_avg_test_runtime(test, self.task_runtime_stats)
            if avg_test_runtime:
                LOGGER.debug("Avg test runtime", test=test, runtime=avg_test_runtime)

                timeout = _calculate_timeout(avg_test_runtime)
                exec_timeout = _calculate_exec_timeout(self.repeat_config, avg_test_runtime)
                LOGGER.debug("Using timeout overrides", exec_timeout=exec_timeout, timeout=timeout)
                timeout_info = TimeoutInfo.overridden(exec_timeout, timeout)

                LOGGER.debug("Override runtime for test", test=test, timeout=timeout_info)
                return timeout_info

        return TimeoutInfo.default_timeout()

    def generate_name(self, index: int) -> str:
        """
        Generate a subtask name.

        :param index: Index of subtask.
        :return: Name to use for generated sub-task.
        """
        prefix = self.generate_config.task_prefix
        task_name = self.task_info.display_task_name
        return name_generated_task(f"{prefix}:{task_name}", index, len(self.task_info.tests),
                                   self.generate_config.run_build_variant)

    def create_task(self, index: int, test_name: str) -> Task:
        """
        Create the task configuration for the given test using the given index.

        :param index: Index of sub-task being created.
        :param test_name: Name of test that should be executed.
        :return: Configuration for generating the specified task.
        """
        resmoke_args = self.task_info.resmoke_args

        sub_task_name = self.generate_name(index)
        LOGGER.debug("Generating sub-task", sub_task=sub_task_name)

        test_unix_style = test_name.replace('\\', '/')
        run_tests_vars = {
            "resmoke_args":
                f"{resmoke_args} {self.repeat_config.generate_resmoke_options()} {test_unix_style}"
        }

        timeout = self.generate_timeouts(test_name)
        commands = resmoke_commands("run tests", run_tests_vars, timeout,
                                    self.task_info.require_multiversion)
        dependencies = {TaskDependency(TASK_WITH_ARTIFACTS)}

        return Task(sub_task_name, commands, dependencies)


class EvergreenFileChangeDetector(FileChangeDetector):
    """A file changes detector for detecting test change in evergreen."""

    def __init__(self, task_id: str, evg_api: EvergreenApi) -> None:
        """
        Create a new evergreen file change detector.

        :param task_id: Id of task being run under.
        :param evg_api: Evergreen API client.
        """
        self.task_id = task_id
        self.evg_api = evg_api

    def create_revision_map(self, repos: List[Repo]) -> RevisionMap:
        """
        Create a map of the repos and the given revisions to diff against.

        :param repos: List of repos being tracked.
        :return: Map of repositories and revisions to diff against.
        """
        return generate_revision_map_from_manifest(repos, self.task_id, self.evg_api)


class GenerateBurnInExecutor(BurnInExecutor):
    """A burn-in executor that generates tasks."""

    # pylint: disable=too-many-arguments
    def __init__(self, generate_config: GenerateConfig, repeat_config: RepeatConfig,
                 evg_api: EvergreenApi, generate_tasks_file: Optional[str] = None,
                 history_end_date: Optional[datetime] = None) -> None:
        """
        Create a new generate burn-in executor.

        :param generate_config: Configuration for how to generate tasks.
        :param repeat_config: Configuration for how tests should be repeated.
        :param evg_api: Evergreen API client.
        :param generate_tasks_file: File to write generated task configuration to.
        :param history_end_date: End date of range to query for historic test data.
        """
        self.generate_config = generate_config
        self.repeat_config = repeat_config
        self.evg_api = evg_api
        self.generate_tasks_file = generate_tasks_file
        self.history_end_date = history_end_date if history_end_date else datetime.utcnow()\
            .replace(microsecond=0)

    def get_task_runtime_history(self, task: str) -> List[TestRuntime]:
        """
        Query the runtime history of the specified task.

        :param task: Task to query.
        :return: List of runtime histories for all tests in specified task.
        """
        try:
            project = self.generate_config.project
            variant = self.generate_config.build_variant
            end_date = self.history_end_date
            start_date = end_date - timedelta(days=AVG_TEST_RUNTIME_ANALYSIS_DAYS)
            test_stats = HistoricTaskData.from_evg(self.evg_api, project, start_date=start_date,
                                                   end_date=end_date, task=task, variant=variant)
            return test_stats.get_tests_runtimes()
        except requests.HTTPError as err:
            if err.response.status_code == requests.codes.SERVICE_UNAVAILABLE:
                # Evergreen may return a 503 when the service is degraded.
                # We fall back to returning no test history
                return []
            else:
                raise

    def create_generated_tasks(self, tests_by_task: Dict[str, TaskInfo]) -> Set[Task]:
        """
        Create generate.tasks configuration for the the given tests and tasks.

        :param tests_by_task: Dictionary of tasks and test to generate configuration for.
        :return: Shrub tasks containing the configuration for generating specified tasks.
        """
        tasks: Set[Task] = set()
        for task in sorted(tests_by_task):
            task_info = tests_by_task[task]
            task_runtime_stats = self.get_task_runtime_history(task_info.display_task_name)
            task_generator = TaskGenerator(self.generate_config, self.repeat_config, task_info,
                                           task_runtime_stats)

            for index, test_name in enumerate(task_info.tests):
                tasks.add(task_generator.create_task(index, test_name))

        return tasks

    def get_existing_tasks(self) -> Optional[Set[ExistingTask]]:
        """Get any existing tasks that should be included in the generated display task."""
        if self.generate_config.include_gen_task:
            return {ExistingTask(BURN_IN_TESTS_GEN_TASK)}
        return None

    def add_config_for_build_variant(self, build_variant: BuildVariant,
                                     tests_by_task: Dict[str, TaskInfo]) -> None:
        """
        Add configuration for generating tasks to the given build variant.

        :param build_variant: Build variant to update.
        :param tests_by_task: Tasks and tests to update.
        """
        tasks = self.create_generated_tasks(tests_by_task)
        build_variant.display_task(BURN_IN_TESTS_TASK, tasks,
                                   execution_existing_tasks=self.get_existing_tasks())

    def create_generate_tasks_configuration(self, tests_by_task: Dict[str, TaskInfo]) -> str:
        """
        Create the configuration with the configuration to generate the burn_in tasks.

        :param tests_by_task: Dictionary of tasks and test to generate.
        :return: Configuration to use to create generated tasks.
        """
        build_variant = BuildVariant(self.generate_config.run_build_variant)
        self.add_config_for_build_variant(build_variant, tests_by_task)

        shrub_project = ShrubProject.empty()
        shrub_project.add_build_variant(build_variant)

        if not validate_task_generation_limit(shrub_project):
            sys.exit(1)

        return shrub_project.json()

    def execute(self, tests_by_task: Dict[str, TaskInfo]) -> None:
        """
        Execute the given tests in the given tasks.

        :param tests_by_task: Dictionary of tasks to run with tests to run in each.
        """
        json_text = self.create_generate_tasks_configuration(tests_by_task)
        assert self.generate_tasks_file is not None
        if self.generate_tasks_file:
            write_file(self.generate_tasks_file, json_text)


# pylint: disable=too-many-arguments
def burn_in(task_id: str, build_variant: str, generate_config: GenerateConfig,
            repeat_config: RepeatConfig, evg_api: EvergreenApi, evg_conf: EvergreenProjectConfig,
            repos: List[Repo], generate_tasks_file: str) -> None:
    """
    Run burn_in_tests.

    :param task_id: Id of task running.
    :param build_variant: Build variant to run against.
    :param generate_config: Configuration for how to generate tasks.
    :param repeat_config: Configuration for how to repeat tests.
    :param evg_api: Evergreen API client.
    :param evg_conf: Evergreen project configuration.
    :param repos: Git repos containing changes.
    :param generate_tasks_file: File to write generate tasks configuration to.
    """
    change_detector = EvergreenFileChangeDetector(task_id, evg_api)
    executor = GenerateBurnInExecutor(generate_config, repeat_config, evg_api, generate_tasks_file)

    burn_in_orchestrator = BurnInOrchestrator(change_detector, executor, evg_conf)
    burn_in_orchestrator.burn_in(repos, build_variant)


@click.command()
@click.option("--generate-tasks-file", "generate_tasks_file", default=None, metavar='FILE',
              help="Run in 'generate.tasks' mode. Store task config to given file.")
@click.option("--build-variant", "build_variant", default=DEFAULT_VARIANT, metavar='BUILD_VARIANT',
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
@click.option("--verbose", "verbose", default=False, is_flag=True, help="Enable extra logging.")
@click.option("--task_id", "task_id", required=True, metavar='TASK_ID',
              help="The evergreen task id.")
# pylint: disable=too-many-arguments,too-many-locals
def main(build_variant: str, run_build_variant: str, distro: str, project: str,
         generate_tasks_file: str, repeat_tests_num: Optional[int], repeat_tests_min: Optional[int],
         repeat_tests_max: Optional[int], repeat_tests_secs: Optional[int], evg_api_config: str,
         verbose: bool, task_id: str):
    """
    Run new or changed tests in repeated mode to validate their stability.

    burn_in_tests detects jstests that are new or changed since the last git command and then
    runs those tests in a loop to validate their reliability.

    The `--origin-rev` argument allows users to specify which revision should be used as the last
    git command to compare against to find changed files. If the `--origin-rev` argument is provided,
    we find changed files by comparing your latest changes to this revision. If not provided, we
    find changed test files by comparing your latest changes to HEAD. The revision provided must
    be a revision that exists in the mongodb repository.

    The `--repeat-*` arguments allow configuration of how burn_in_tests repeats tests. Tests can
    either be repeated a specified number of times with the `--repeat-tests` option, or they can
    be repeated for a certain time period with the `--repeat-tests-secs` option.

    Specifying the `--generate-tasks-file`, burn_in_tests will run generate a configuration
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
    :param repeat_tests_num: Repeat each test this number of times.
    :param repeat_tests_min: Repeat each test at least this number of times.
    :param repeat_tests_max: Once this number of repetitions has been reached, stop repeating.
    :param repeat_tests_secs: Continue repeating tests for this number of seconds.
    :param evg_api_config: Location of configuration file to connect to evergreen.
    :param verbose: Log extra debug information.
    :param task_id: Id of evergreen task being run in.
    """
    _configure_logging(verbose)

    repeat_config = RepeatConfig(repeat_tests_secs=repeat_tests_secs,
                                 repeat_tests_min=repeat_tests_min,
                                 repeat_tests_max=repeat_tests_max,
                                 repeat_tests_num=repeat_tests_num)  # yapf: disable

    repos = [Repo(x) for x in DEFAULT_REPO_LOCATIONS if os.path.isdir(x)]
    evg_conf = parse_evergreen_file(EVERGREEN_FILE)
    evg_api = RetryingEvergreenApi.get_api(config_file=evg_api_config)

    generate_config = GenerateConfig(build_variant=build_variant,
                                     run_build_variant=run_build_variant,
                                     distro=distro,
                                     project=project,
                                     task_id=task_id)  # yapf: disable
    generate_config.validate(evg_conf)

    burn_in(task_id, build_variant, generate_config, repeat_config, evg_api, evg_conf, repos,
            generate_tasks_file)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
