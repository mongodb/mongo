#!/usr/bin/env python3
"""Command line utility for determining what jstests should run for the given changed files."""
import os
import re
import sys
from datetime import datetime, timedelta
from functools import partial
from typing import Any, Dict, List, Set, Optional

import click
import inject
import structlog
from pydantic import BaseModel
from structlog.stdlib import LoggerFactory
from git import Repo
from evergreen.api import EvergreenApi, RetryingEvergreenApi

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
# Get relative imports to work when the package is not installed on the PYTHONPATH.
from buildscripts.patch_builds.change_data import find_changed_files_in_repos
from buildscripts.patch_builds.evg_change_data import generate_revision_map_from_manifest
from buildscripts.patch_builds.selected_tests.selected_tests_client import SelectedTestsClient
from buildscripts.task_generation.evg_config_builder import EvgConfigBuilder
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.generated_config import GeneratedConfiguration
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyConfig
from buildscripts.task_generation.suite_split import SuiteSplitParameters, SuiteSplitConfig
from buildscripts.task_generation.suite_split_strategies import SplitStrategy, FallbackStrategy, \
    greedy_division, round_robin_fallback
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions
from buildscripts.task_generation.task_types.resmoke_tasks import ResmokeGenTaskParams
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.fileops import read_yaml_file
from buildscripts.burn_in_tests import DEFAULT_REPO_LOCATIONS, create_task_list_for_tests, \
    TaskInfo
from buildscripts.ciconfig.evergreen import (
    EvergreenProjectConfig,
    ResmokeArgs,
    Task,
    parse_evergreen_file,
    Variant,
)
from buildscripts.patch_builds.selected_tests.selected_tests_service import SelectedTestsService

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.getLogger(__name__)

DEFAULT_TEST_SUITE_DIR = os.path.join("buildscripts", "resmokeconfig", "suites")
TASK_ID_EXPANSION = "task_id"
EVERGREEN_FILE = "etc/evergreen.yml"
EVG_CONFIG_FILE = ".evergreen.yml"
SELECTED_TESTS_CONFIG_DIR = "generated_resmoke_config"
RELATION_THRESHOLD = 0
LOOKBACK_DURATION_DAYS = 14

COMPILE_TASK_PATTERN = re.compile(".*compile.*")
CONCURRENCY_TASK_PATTERN = re.compile("concurrency.*")
INTEGRATION_TASK_PATTERN = re.compile("integration.*")
FUZZER_TASK_PATTERN = re.compile(".*fuzz.*")
GENERATE_TASK_PATTERN = re.compile("burn_in.*")
MULTIVERSION_TASK_PATTERN = re.compile(".*multiversion.*")
LINT_TASK_PATTERN = re.compile("lint.*")
STITCH_TASK_PATTERN = re.compile("stitch.*")
EXCLUDE_TASK_PATTERNS = [
    COMPILE_TASK_PATTERN, CONCURRENCY_TASK_PATTERN, INTEGRATION_TASK_PATTERN, FUZZER_TASK_PATTERN,
    GENERATE_TASK_PATTERN, LINT_TASK_PATTERN, MULTIVERSION_TASK_PATTERN, STITCH_TASK_PATTERN
]

CPP_TASK_NAMES = [
    "dbtest",
    "idl_tests",
    "run_unittests",
]
PUBLISH_TASK_NAMES = [
    "package",
    "publish_packages",
    "push",
]
PYTHON_TESTS = ["buildscripts_test"]
EXCLUDE_TASK_LIST = [
    *CPP_TASK_NAMES,
    *PYTHON_TESTS,
    *PUBLISH_TASK_NAMES,
]
POSSIBLE_RUN_TASK_FUNCS = [
    "generate resmoke tasks",
    "generate randomized multiversion tasks",
    "run tests",
    "generate explicit multiversion tasks",
]


class EvgExpansions(BaseModel):
    """
    Evergreen expansions needed for selected tests.

    task_id: ID of task being run.
    task_name: Name of task being run.
    build_variant: Name of build variant being run on.
    build_id: ID of build being run.
    is_patch: Is this task run as part of a patch build.
    project: Evergreen project being run.
    revision: git revision being run against.
    version_id: ID of version being run.
    """

    task_id: str
    task_name: str
    build_variant: str
    build_id: str
    is_patch: Optional[bool] = None
    project: str
    revision: str
    version_id: str

    @classmethod
    def from_yaml_file(cls, path: str) -> "EvgExpansions":
        """Read the generation configuration from the given file."""
        return cls(**read_yaml_file(path))

    def build_gen_task_options(self) -> GenTaskOptions:
        """Build options needed to generate tasks."""
        return GenTaskOptions(create_misc_suite=False,
                              generated_config_dir=SELECTED_TESTS_CONFIG_DIR, is_patch=self.is_patch
                              or False, use_default_timeouts=False)

    def build_suite_split_config(self, start_date: datetime,
                                 end_date: datetime) -> SuiteSplitConfig:
        """
        Build options need to split suite into sub-suites.

        :param start_date: Start date to look at historic results.
        :param end_date: End date to look at historic results.
        :return: Options for splitting suites.
        """
        return SuiteSplitConfig(
            evg_project=self.project,
            target_resmoke_time=60,
            max_sub_suites=5,
            max_tests_per_suite=100,
            start_date=start_date,
            end_date=end_date,
            include_build_variant_in_name=True,
        )

    def get_config_location(self) -> str:
        """Get the location the generated configuration will be stored."""
        return f"{self.build_variant}/{self.revision}/generate_tasks/{self.task_name}-{self.build_id}.tgz"


class TaskConfigService:
    """Service for generating selected tests task configuration."""

    @staticmethod
    def get_evg_task_config(task: Task, build_variant_config: Variant) -> Dict[str, Any]:
        """
        Look up task config of the task to be generated.

        :param task: Task to get info for.
        :param build_variant_config: Config of build variant to collect task info from.
        :return: Task configuration values.
        """
        LOGGER.info("Calculating evg_task_config values for task", task=task.name)
        task_vars = {}
        for run_task_func in POSSIBLE_RUN_TASK_FUNCS:
            task_def = task.find_func_command(run_task_func)
            if task_def:
                task_vars = task_def["vars"]
                break

        suite_name = ResmokeArgs.get_arg(task_vars["resmoke_args"], "suites")
        if suite_name:
            task_vars.update({"suite": suite_name})

        # the suites argument will run all tests in a suite even when individual
        # tests are specified in resmoke_args, so we remove it
        resmoke_args_without_suites = ResmokeArgs.remove_arg(task_vars["resmoke_args"], "suites")
        task_vars["resmoke_args"] = resmoke_args_without_suites

        task_name = task.name[:-4] if task.name.endswith("_gen") else task.name
        return {
            "task_name": task_name,
            "build_variant": build_variant_config.name,
            **task_vars,
            "large_distro_name": build_variant_config.expansion("large_distro_name"),
        }

    def get_task_configs_for_test_mappings(self, tests_by_task: Dict[str, TaskInfo],
                                           build_variant_config: Variant) -> Dict[str, dict]:
        """
        For test mappings, generate a dict containing task names and their config settings.

        :param tests_by_task: Dictionary of tests and tasks to run.
        :param build_variant_config: Config of build variant to collect task info from.
        :return: Dict of task names and their config settings.
        """
        evg_task_configs = {}
        for task_name, test_list_info in tests_by_task.items():
            task = _find_task(build_variant_config, task_name)
            if task and not _exclude_task(task):
                evg_task_config = self.get_evg_task_config(task, build_variant_config)
                evg_task_config.update({"selected_tests_to_run": set(test_list_info.tests)})
                evg_task_configs[task.name] = evg_task_config

        return evg_task_configs

    def get_task_configs_for_task_mappings(self, related_tasks: List[str],
                                           build_variant_config: Variant) -> Dict[str, dict]:
        """
        For task mappings, generate a dict containing task names and their config settings.

        :param related_tasks: List of tasks to run.
        :param build_variant_config: Config of build variant to collect task info from.
        :return: Dict of task names and their config settings.
        """
        evg_task_configs = {}
        for task_name in related_tasks:
            task = _find_task(build_variant_config, task_name)
            if task and not _exclude_task(task):
                evg_task_config = self.get_evg_task_config(task, build_variant_config)
                evg_task_configs[task.name] = evg_task_config

        return evg_task_configs


def _exclude_task(task: Task) -> bool:
    """
    Check whether a task should be excluded.

    :param task: Task to get info for.
    :return: True if this task should be excluded.
    """
    if task.name in EXCLUDE_TASK_LIST or any(
            regex.match(task.name) for regex in EXCLUDE_TASK_PATTERNS):
        LOGGER.debug("Excluding task from analysis because it is not a jstest", task=task.name)
        return True
    return False


def _find_task(build_variant_config: Variant, task_name: str) -> Task:
    """
    Look up shrub config for task.

    :param build_variant_config: Config of build variant to collect task info from.
    :param task_name: Name of task to get info for.
    :return: Task configuration.
    """
    task = build_variant_config.get_task(task_name)
    if not task:
        task = build_variant_config.get_task(task_name + "_gen")
    return task


def _remove_repo_path_prefix(file_path: str) -> str:
    """
    Remove the repo path prefix from the filepath.

    :param file_path: Path of the changed file.
    :return: Path of the changed file without prefix.
    """
    for repo_path in DEFAULT_REPO_LOCATIONS:
        if repo_path != ".":
            if repo_path.startswith("./"):
                repo_path = repo_path[2:]
                file_path = re.sub(repo_path + "/", '', file_path)
    return file_path


def filter_set(item: str, input_set: Set[str]) -> bool:
    """
    Filter to determine if the given item is in the given set.

    :param item: Item to search for.
    :param input_set: Set to search.
    :return: True if the item is contained in the list.
    """
    return item in input_set


class SelectedTestsOrchestrator:
    """Orchestrator for generating selected test builds."""

    # pylint: disable=too-many-arguments
    @inject.autoparams()
    def __init__(self, evg_api: EvergreenApi, evg_conf: EvergreenProjectConfig,
                 selected_tests_service: SelectedTestsService,
                 task_config_service: TaskConfigService, evg_expansions: EvgExpansions) -> None:
        """
        Initialize the orchestrator.

        :param evg_api: Evergreen API client.
        :param evg_conf: Evergreen Project configuration.
        :param selected_tests_service: Selected tests service.
        :param task_config_service: Task Config service.
        :param evg_expansions: Evergreen expansions.
        """
        self.evg_api = evg_api
        self.evg_conf = evg_conf
        self.selected_tests_service = selected_tests_service
        self.task_config_service = task_config_service
        self.evg_expansions = evg_expansions

    def find_changed_files(self, repos: List[Repo], task_id: str) -> Set[str]:
        """
        Determine what files have changed in the given repos.

        :param repos: List of git repos to query.
        :param task_id: ID of task being run.
        :return: Set of files that contain changes.
        """
        revision_map = generate_revision_map_from_manifest(repos, task_id, self.evg_api)
        changed_files = find_changed_files_in_repos(repos, revision_map)
        changed_files = {_remove_repo_path_prefix(file_path) for file_path in changed_files}
        changed_files = {
            file_path
            for file_path in changed_files if not file_path.startswith("src/third_party")
        }
        LOGGER.info("Found changed files", files=changed_files)
        return changed_files

    def get_task_config(self, build_variant_config: Variant,
                        changed_files: Set[str]) -> Dict[str, Dict]:
        """
        Get task configurations for the tasks to be generated.

        :param build_variant_config: Config of build variant to collect task info from.
        :param changed_files: Set of changed_files.
        :return: Task configurations.
        """
        existing_tasks = self.get_existing_tasks(self.evg_expansions.version_id,
                                                 build_variant_config.name)
        task_configs = {}

        related_test_files = self.selected_tests_service.find_selected_test_files(changed_files)
        LOGGER.info("related test files found", related_test_files=related_test_files,
                    variant=build_variant_config.name)

        if related_test_files:
            tests_by_task = create_task_list_for_tests(related_test_files,
                                                       build_variant_config.name, self.evg_conf)
            LOGGER.info("tests and tasks found", tests_by_task=tests_by_task)
            tests_by_task = {
                task: tests
                for task, tests in tests_by_task.items() if task not in existing_tasks
            }

            test_mapping_task_configs = self.task_config_service.get_task_configs_for_test_mappings(
                tests_by_task, build_variant_config)
            task_configs.update(test_mapping_task_configs)

        related_tasks = self.selected_tests_service.find_selected_tasks(changed_files)
        LOGGER.info("related tasks found", related_tasks=related_tasks,
                    variant=build_variant_config.name)
        related_tasks = {task for task in related_tasks if task not in existing_tasks}
        if related_tasks:
            task_mapping_task_configs = self.task_config_service.get_task_configs_for_task_mappings(
                list(related_tasks), build_variant_config)
            # task_mapping_task_configs will overwrite test_mapping_task_configs
            # because task_mapping_task_configs will run all tests rather than a subset of tests
            # and we should err on the side of running all tests
            task_configs.update(task_mapping_task_configs)

        return task_configs

    def get_existing_tasks(self, version_id: str, build_variant: str) -> Set[str]:
        """
        Get the set of tasks that already exist in the given build.

        :param version_id: ID of version to query.
        :param build_variant: Name of build variant to query.
        :return: Set of task names that already exist in the specified build.
        """
        version = self.evg_api.version_by_id(version_id)

        try:
            build = version.build_by_variant(build_variant)
        except KeyError:
            LOGGER.debug("No build exists on this build variant for this version yet",
                         variant=build_variant)
            return set()

        if build:
            tasks_already_in_build = build.get_tasks()
            return {task.display_name for task in tasks_already_in_build}

        return set()

    def generate_build_variant(self, build_variant_config: Variant, changed_files: Set[str],
                               builder: EvgConfigBuilder) -> None:
        """
        Generate the selected tasks on the specified build variant.

        :param build_variant_config: Configuration of build variant to generate.
        :param changed_files: List of file changes to determine what to run.
        :param builder: Builder to create new configuration.
        """
        build_variant_name = build_variant_config.name
        LOGGER.info("Generating build variant", build_variant=build_variant_name)
        task_configs = self.get_task_config(build_variant_config, changed_files)

        for task_config in task_configs.values():
            test_filter = None
            if "selected_tests_to_run" in task_config:
                test_filter = partial(filter_set, input_set=task_config["selected_tests_to_run"])
            split_params = SuiteSplitParameters(
                build_variant=build_variant_name,
                task_name=task_config["task_name"],
                suite_name=task_config.get("suite", task_config["task_name"]),
                filename=task_config.get("suite", task_config["task_name"]),
                test_file_filter=test_filter,
                is_asan=build_variant_config.is_asan_build(),
            )
            gen_params = ResmokeGenTaskParams(
                use_large_distro=task_config.get("use_large_distro", False),
                large_distro_name=task_config.get("large_distro_name"),
                require_multiversion=task_config.get("require_multiversion"),
                repeat_suites=task_config.get("repeat_suites", 1),
                resmoke_args=task_config["resmoke_args"],
                resmoke_jobs_max=task_config.get("resmoke_jobs_max"),
                config_location=self.evg_expansions.get_config_location(),
            )
            builder.generate_suite(split_params, gen_params)

    def generate(self, repos: List[Repo], task_id: str) -> None:
        """
        Build and generate the configuration to create selected tests.

        :param repos: List of git repos containing changes to check.
        :param task_id: ID of task being run.
        """
        changed_files = self.find_changed_files(repos, task_id)
        generated_config = self.generate_version(changed_files)
        generated_config.write_all_to_dir(SELECTED_TESTS_CONFIG_DIR)

    def generate_version(self, changed_files: Set[str]) -> GeneratedConfiguration:
        """
        Generate selected tests configuration for the given file changes.

        :param changed_files: Set of files that contain changes.
        :return: Configuration to generate selected-tests tasks.
        """
        builder = EvgConfigBuilder()  # pylint: disable=no-value-for-parameter
        for build_variant_config in self.evg_conf.get_required_variants():
            self.generate_build_variant(build_variant_config, changed_files, builder)

        return builder.build("selected_tests_config.json")


@click.command()
@click.option("--verbose", "verbose", default=False, is_flag=True, help="Enable extra logging.")
@click.option(
    "--expansion-file",
    "expansion_file",
    type=str,
    required=True,
    help="Location of expansions file generated by evergreen.",
)
@click.option(
    "--evg-api-config",
    "evg_api_config",
    default=EVG_CONFIG_FILE,
    metavar="FILE",
    help="Configuration file with connection info for Evergreen API.",
)
@click.option(
    "--selected-tests-config",
    "selected_tests_config",
    required=True,
    metavar="FILE",
    help="Configuration file with connection info for selected tests service.",
)
def main(
        verbose: bool,
        expansion_file: str,
        evg_api_config: str,
        selected_tests_config: str,
):
    """
    Select tasks to be run based on changed files in a patch build.

    :param verbose: Log extra debug information.
    :param expansion_file: Configuration file.
    :param evg_api_config: Location of configuration file to connect to evergreen.
    :param selected_tests_config: Location of config file to connect to elected-tests service.
    """
    enable_logging(verbose)

    end_date = datetime.utcnow().replace(microsecond=0)
    start_date = end_date - timedelta(days=LOOKBACK_DURATION_DAYS)

    evg_expansions = EvgExpansions.from_yaml_file(expansion_file)

    def dependencies(binder: inject.Binder) -> None:
        binder.bind(EvgExpansions, evg_expansions)
        binder.bind(EvergreenApi, RetryingEvergreenApi.get_api(config_file=evg_api_config))
        binder.bind(EvergreenProjectConfig, parse_evergreen_file(EVERGREEN_FILE))
        binder.bind(SelectedTestsClient, SelectedTestsClient.from_file(selected_tests_config))
        binder.bind(SuiteSplitConfig, evg_expansions.build_suite_split_config(start_date, end_date))
        binder.bind(SplitStrategy, greedy_division)
        binder.bind(FallbackStrategy, round_robin_fallback)
        binder.bind(GenTaskOptions, evg_expansions.build_gen_task_options())
        binder.bind(GenerationConfiguration, GenerationConfiguration.from_yaml_file())
        binder.bind(ResmokeProxyConfig,
                    ResmokeProxyConfig(resmoke_suite_dir=DEFAULT_TEST_SUITE_DIR))

    inject.configure(dependencies)

    repos = [Repo(x) for x in DEFAULT_REPO_LOCATIONS if os.path.isdir(x)]
    selected_tests = SelectedTestsOrchestrator()  # pylint: disable=no-value-for-parameter
    selected_tests.generate(repos, evg_expansions.task_id)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
