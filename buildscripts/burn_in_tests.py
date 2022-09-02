#!/usr/bin/env python3
"""Command line utility for determining what jstests have been added or modified."""
import copy
import logging
import os.path
import shlex
import subprocess
import sys
from abc import ABC, abstractmethod
from collections import defaultdict
from typing import Optional, Set, Tuple, List, Dict, NamedTuple

import click
import yaml
from git import Repo
from pydantic import BaseModel
import structlog
from structlog.stdlib import LoggerFactory

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.patch_builds.change_data import generate_revision_map, \
    RevisionMap, find_changed_files_in_repos
import buildscripts.resmokelib.parser
from buildscripts.resmokelib.suitesconfig import create_test_membership_map, get_suites
from buildscripts.resmokelib.utils import default_if_none, globstar
from buildscripts.ciconfig.evergreen import parse_evergreen_file, EvergreenProjectConfig, Variant, VariantTask
# pylint: enable=wrong-import-position

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.getLogger(__name__)
EXTERNAL_LOGGERS = {
    "evergreen",
    "git",
    "urllib3",
}

DEFAULT_VARIANT = "enterprise-rhel-80-64-bit-dynamic-required"
ENTERPRISE_MODULE_PATH = "src/mongo/db/modules/enterprise"
DEFAULT_REPO_LOCATIONS = [".", f"./{ENTERPRISE_MODULE_PATH}"]
REPEAT_SUITES = 2
EVERGREEN_FILE = "etc/evergreen.yml"
# The executor_file and suite_files defaults are required to make the suite resolver work
# correctly.
SELECTOR_FILE = "etc/burn_in_tests.yml"
SUITE_FILES = ["with_server"]

SUPPORTED_TEST_KINDS = ("fsm_workload_test", "js_test", "json_schema_test",
                        "multi_stmt_txn_passthrough", "parallel_fsm_workload_test",
                        "all_versions_js_test")


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


def is_file_a_test_file(file_path: str) -> bool:
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


def filter_tests(tests: Set[str], exclude_tests: List[str]) -> Set[str]:
    """
    Exclude tests which have been denylisted.

    :param tests: Set of tests to filter.
    :param exclude_tests: Tests to filter out.
    :return: Set of tests with exclude_tests filtered out.
    """
    tests = {test for test in tests if test.strip()}
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
        resmoke_vars = task_def.generate_resmoke_tasks_command.get("vars", {})
        if "use_large_distro" in resmoke_vars:
            evg_build_variant = _get_evg_build_variant_by_name(evg_proj_config, build_variant)
            if "large_distro_name" in evg_build_variant.raw["expansions"]:
                return evg_build_variant.raw["expansions"]["large_distro_name"]

    return task.run_on[0]


class TaskInfo(NamedTuple):
    """
    Information about tests to run under a specific Task.

    display_task_name: Display name of task.
    suite: Name of resmoke.pu suite that runs in this task.
    resmoke_args: Arguments to provide to resmoke on task invocation.
    tests: List of tests to run as part of task.
    require_multiversion_setup: Requires downloading Multiversion binaries.
    distro: Evergreen distro task runs on.
    build_variant: Evergreen build variant the task runs on.
    """

    display_task_name: str
    require_multiversion_setup: bool
    suite: str
    resmoke_args: str
    tests: List[str]
    distro: str
    build_variant: str

    @classmethod
    def from_task(cls, task: VariantTask, tests_by_suite: Dict[str, List[str]],
                  evg_proj_config: EvergreenProjectConfig, build_variant: str) -> "TaskInfo":
        """
        Gather the information needed to run the given task.

        :param task: Task to be run.
        :param tests_by_suite: Dict of suites.
        :param evg_proj_config: Evergreen project configuration.
        :param build_variant: Build variant task will be run on.
        :return: Dictionary of information needed to run task.
        """
        suite = task.get_suite_name()
        return cls(
            display_task_name=_get_task_name(task), resmoke_args=task.resmoke_args, suite=suite,
            tests=tests_by_suite[suite],
            require_multiversion_setup=task.require_multiversion_setup(),
            distro=_distro_to_run_task_on(task, evg_proj_config,
                                          build_variant), build_variant=build_variant)


def create_task_list(evergreen_conf: EvergreenProjectConfig, build_variant: str,
                     tests_by_suite: Dict[str, List[str]],
                     exclude_tasks: [str]) -> Dict[str, TaskInfo]:
    """
    Find associated tasks for the specified build_variant and suites.

    :param evergreen_conf: Evergreen configuration for project.
    :param build_variant: Build variant to select tasks from.
    :param tests_by_suite: Suites to be run.
    :param exclude_tasks: Tasks to exclude.
    :return: Dict of tasks to run with run configuration.
    """
    log = LOGGER.bind(build_variant=build_variant)

    log.debug("creating task list for suites", suites=tests_by_suite, exclude_tasks=exclude_tasks)
    evg_build_variant = _get_evg_build_variant_by_name(evergreen_conf, build_variant)

    # Find all the build variant tasks.
    exclude_tasks_set = set(exclude_tasks)
    all_variant_tasks = {
        task.name: task
        for task in evg_build_variant.tasks if task.name not in exclude_tasks_set and (
            task.is_run_tests_task or task.is_generate_resmoke_task)
    }

    # Return the list of tasks to run for the specified suite.
    task_list = {
        task_name: TaskInfo.from_task(task, tests_by_suite, evergreen_conf, build_variant)
        for task_name, task in all_variant_tasks.items() if task.get_suite_name() in tests_by_suite
    }

    log.debug("Found task list", task_list=task_list)
    return task_list


def _set_resmoke_cmd(repeat_config: RepeatConfig, resmoke_args: [str]) -> [str]:
    """Build the resmoke command, if a resmoke.py command wasn't passed in."""
    new_args = [sys.executable, "buildscripts/resmoke.py", "run"]
    if resmoke_args:
        new_args += copy.deepcopy(resmoke_args)

    new_args += repeat_config.generate_resmoke_options().split()
    LOGGER.debug("set resmoke command", new_args=new_args)
    return new_args


def create_task_list_for_tests(changed_tests: Set[str], build_variant: str,
                               evg_conf: EvergreenProjectConfig,
                               exclude_suites: Optional[List] = None,
                               exclude_tasks: Optional[List] = None) -> Dict[str, TaskInfo]:
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

    suites = get_suites(suite_names_or_paths=SUITE_FILES, test_files=changed_tests)
    LOGGER.debug("Found suites to run", suites=suites)

    tests_by_executor = create_executor_list(suites, exclude_suites)
    LOGGER.debug("tests_by_executor", tests_by_executor=tests_by_executor)

    return create_task_list(evg_conf, build_variant, tests_by_executor, exclude_tasks)


def create_tests_by_task(build_variant: str, evg_conf: EvergreenProjectConfig,
                         changed_tests: Set[str],
                         install_dir: Optional[str]) -> Dict[str, TaskInfo]:
    """
    Create a list of tests by task.

    :param build_variant: Build variant to collect tasks from.
    :param evg_conf: Evergreen configuration.
    :param changed_tests: Set of changed test files.
    :return: Tests by task.
    """
    exclude_suites, exclude_tasks, exclude_tests = find_excludes(SELECTOR_FILE)
    evg_build_variant = _get_evg_build_variant_by_name(evg_conf, build_variant)
    if not evg_build_variant.is_enterprise_build():
        exclude_tests.append(f"{ENTERPRISE_MODULE_PATH}/**/*")
    changed_tests = filter_tests(changed_tests, exclude_tests)

    run_options = ""
    if install_dir is not None:
        run_options = f"--installDir={shlex.quote(install_dir)}"
    buildscripts.resmokelib.parser.set_run_options(run_options)

    if changed_tests:
        return create_task_list_for_tests(changed_tests, build_variant, evg_conf, exclude_suites,
                                          exclude_tasks)

    LOGGER.info("No new or modified tests found.")
    return {}


def run_tests(tests_by_task: Dict[str, TaskInfo], resmoke_cmd: [str]) -> None:
    """
    Run the given tests locally.

    This function will exit with a non-zero return code on test failure.

    :param tests_by_task: Dictionary of tests to run.
    :param resmoke_cmd: Parameter to use when calling resmoke.
    """
    for task in sorted(tests_by_task):
        log = LOGGER.bind(task=task)
        new_resmoke_cmd = copy.deepcopy(resmoke_cmd)
        new_resmoke_cmd.extend(shlex.split(tests_by_task[task].resmoke_args))
        new_resmoke_cmd.extend(tests_by_task[task].tests)
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
        stream=sys.stderr,
    )
    for log_name in EXTERNAL_LOGGERS:
        logging.getLogger(log_name).setLevel(logging.WARNING)


def _get_evg_build_variant_by_name(evergreen_conf: EvergreenProjectConfig, name: str) -> Variant:
    """
    Get the evergreen build variant by name from the evergreen config file.

    :param evergreen_conf: The evergreen config file.
    :param name: The build variant name to find.
    :return: The evergreen build variant.
    """
    evg_build_variant = evergreen_conf.get_variant(name)
    if not evg_build_variant:
        LOGGER.warning("Build variant not found in evergreen config")
        raise ValueError(f"Build variant ({name} not found in evergreen configuration")

    return evg_build_variant


class FileChangeDetector(ABC):
    """Interface to detect changes to files."""

    @abstractmethod
    def create_revision_map(self, repos: List[Repo]) -> RevisionMap:
        """
        Create a map of the repos and the given revisions to diff against.

        :param repos: List of repos being tracked.
        :return: Map of repositories and revisions to diff against.
        """
        raise NotImplementedError()

    def find_changed_tests(self, repos: List[Repo]) -> Set[str]:
        """
        Find the changed tests.

        Use git to find which test files have changed in this patch.
        The returned file paths are in normalized form (see os.path.normpath(path)).

        :param repos: List of repos containing changed files.
        :return: Set of changed tests.
        """
        revision_map = self.create_revision_map(repos)
        LOGGER.info("Calculated revision map", revision_map=revision_map)

        changed_files = find_changed_files_in_repos(repos, revision_map)
        return {os.path.normpath(path) for path in changed_files if is_file_a_test_file(path)}


class LocalFileChangeDetector(FileChangeDetector):
    """A change detector for detecting changes in a local repository."""

    def __init__(self, origin_rev: Optional[str]) -> None:
        """
        Create a local file change detector.

        :param origin_rev: Git revision to diff against.
        """
        self.origin_rev = origin_rev

    def create_revision_map(self, repos: List[Repo]) -> RevisionMap:
        """
        Create a map of the repos and the given revisions to diff against.

        :param repos: List of repos being tracked.
        :return: Map of repositories and revisions to diff against.
        """
        if self.origin_rev:
            return generate_revision_map(repos, {"mongo": self.origin_rev})

        return {}


class BurnInExecutor(ABC):
    """An interface to execute discovered tests."""

    @abstractmethod
    def execute(self, tests_by_task: Dict[str, TaskInfo]) -> None:
        """
        Execute the given tests in the given tasks.

        :param tests_by_task: Dictionary of tasks to run with tests to run in each.
        """
        raise NotImplementedError()


class NopBurnInExecutor(BurnInExecutor):
    """A burn-in executor that displays results, but doesn't execute."""

    def execute(self, tests_by_task: Dict[str, TaskInfo]) -> None:
        """
        Execute the given tests in the given tasks.

        :param tests_by_task: Dictionary of tasks to run with tests to run in each.
        """
        LOGGER.info("Not running tests due to 'no_exec' option.")
        for task_name, task_info in tests_by_task.items():
            print(task_name)
            for test_name in task_info.tests:
                print(f"- {test_name}")


class LocalBurnInExecutor(BurnInExecutor):
    """A burn-in executor that runs tests on the local machine."""

    def __init__(self, resmoke_args: str, repeat_config: RepeatConfig) -> None:
        """
        Create a new local burn-in executor.

        :param resmoke_args: Resmoke arguments to use for execution.
        :param repeat_config: How tests should be repeated.
        """
        self.resmoke_args = resmoke_args
        self.repeat_config = repeat_config

    def execute(self, tests_by_task: Dict[str, TaskInfo]) -> None:
        """
        Execute the given tests in the given tasks.

        :param tests_by_task: Dictionary of tasks to run with tests to run in each.
        """
        # Populate the config values in order to use the helpers from resmokelib.suitesconfig.
        resmoke_cmd = _set_resmoke_cmd(self.repeat_config, list(self.resmoke_args))
        run_tests(tests_by_task, resmoke_cmd)


class DiscoveredTask(BaseModel):
    """
    Model for a discovered task to run.

    * task_name: Name of discovered task.
    * test_list: List of tests to run under discovered task.
    """

    task_name: str
    test_list: List[str]


class DiscoveredTaskList(BaseModel):
    """Model for a list of discovered tasks."""

    discovered_tasks: List[DiscoveredTask]


class YamlBurnInExecutor(BurnInExecutor):
    """A burn-in executor that outputs discovered tasks as YAML."""

    def execute(self, tests_by_task: Dict[str, TaskInfo]) -> None:
        """
        Report the given tasks and their tests to stdout.

        :param tests_by_task: Dictionary of tasks to run with tests to run in each.
        """
        discovered_tasks = DiscoveredTaskList(discovered_tasks=[
            DiscoveredTask(task_name=task_name, test_list=task_info.tests)
            for task_name, task_info in tests_by_task.items()
        ])
        print(yaml.safe_dump(discovered_tasks.dict()))


class BurnInOrchestrator:
    """Orchestrate the execution of burn_in_tests."""

    def __init__(self, change_detector: FileChangeDetector, burn_in_executor: BurnInExecutor,
                 evg_conf: EvergreenProjectConfig, install_dir: Optional[str]) -> None:
        """
        Create a new orchestrator.

        :param change_detector: Component to use to detect test changes.
        :param burn_in_executor: Components to execute tests.
        :param evg_conf: Evergreen project configuration.
        """
        self.change_detector = change_detector
        self.burn_in_executor = burn_in_executor
        self.evg_conf = evg_conf
        self.install_dir = install_dir

    def burn_in(self, repos: List[Repo], build_variant: str) -> None:
        """
        Execute burn in tests for the given git repositories.

        :param repos: Repositories to check for changes.
        :param build_variant: Build variant to use for task definitions.
        """
        changed_tests = self.change_detector.find_changed_tests(repos)
        LOGGER.info("Found changed tests", files=changed_tests)

        tests_by_task = create_tests_by_task(build_variant, self.evg_conf, changed_tests,
                                             self.install_dir)
        LOGGER.debug("tests and tasks found", tests_by_task=tests_by_task)

        self.burn_in_executor.execute(tests_by_task)


@click.command(context_settings=dict(ignore_unknown_options=True))
@click.option("--no-exec", "no_exec", default=False, is_flag=True,
              help="Do not execute the found tests.")
@click.option("--build-variant", "build_variant", default=DEFAULT_VARIANT, metavar='BUILD_VARIANT',
              help="Tasks to run will be selected from this build variant.")
@click.option("--repeat-tests", "repeat_tests_num", default=None, type=int,
              help="Number of times to repeat tests.")
@click.option("--repeat-tests-min", "repeat_tests_min", default=None, type=int,
              help="The minimum number of times to repeat tests if time option is specified.")
@click.option("--repeat-tests-max", "repeat_tests_max", default=None, type=int,
              help="The maximum number of times to repeat tests if time option is specified.")
@click.option("--repeat-tests-secs", "repeat_tests_secs", default=None, type=int, metavar="SECONDS",
              help="Repeat tests for the given time (in secs).")
@click.option("--yaml", "use_yaml", is_flag=True, default=False,
              help="Output discovered tasks in YAML. Tests will not be run.")
@click.option("--verbose", "verbose", default=False, is_flag=True, help="Enable extra logging.")
@click.option(
    "--origin-rev", "origin_rev", default=None,
    help="The revision in the mongo repo that changes will be compared against if specified.")
@click.option("--install-dir", "install_dir", type=str,
              help="Path to bin directory of a testable installation")
@click.argument("resmoke_args", nargs=-1, type=click.UNPROCESSED)
def main(build_variant: str, no_exec: bool, repeat_tests_num: Optional[int],
         repeat_tests_min: Optional[int], repeat_tests_max: Optional[int],
         repeat_tests_secs: Optional[int], resmoke_args: str, verbose: bool,
         origin_rev: Optional[str], install_dir: Optional[str], use_yaml: bool) -> None:
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

    Any unknown arguments appended to burn_in_tests are further passed to resmoke, e.g.,
    `python buildscripts/burn_in_tests.py --dbpathPrefix /some/other/directory`
    passes `--dbpathPrefix /some/other/directory` to resmoke.
    \f

    :param build_variant: Build variant to query tasks from.
    :param no_exec: Just perform test discover, do not execute the tests.
    :param repeat_tests_num: Repeat each test this number of times.
    :param repeat_tests_min: Repeat each test at least this number of times.
    :param repeat_tests_max: Once this number of repetitions has been reached, stop repeating.
    :param repeat_tests_secs: Continue repeating tests for this number of seconds.
    :param resmoke_args: Arguments to pass through to resmoke.
    :param verbose: Log extra debug information.
    :param origin_rev: The revision that local changes will be compared against.
    """
    _configure_logging(verbose)

    repeat_config = RepeatConfig(repeat_tests_secs=repeat_tests_secs,
                                 repeat_tests_min=repeat_tests_min,
                                 repeat_tests_max=repeat_tests_max,
                                 repeat_tests_num=repeat_tests_num)  # yapf: disable

    repos = [Repo(x) for x in DEFAULT_REPO_LOCATIONS if os.path.isdir(x)]
    evg_conf = parse_evergreen_file(EVERGREEN_FILE)

    change_detector = LocalFileChangeDetector(origin_rev)
    executor = LocalBurnInExecutor(resmoke_args, repeat_config)
    if use_yaml:
        executor = YamlBurnInExecutor()
    elif no_exec:
        executor = NopBurnInExecutor()

    burn_in_orchestrator = BurnInOrchestrator(change_detector, executor, evg_conf, install_dir)
    burn_in_orchestrator.burn_in(repos, build_variant)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
