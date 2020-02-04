#!/usr/bin/env python3
"""Command line utility for determining what jstests should run for the given changed files."""

import logging
import os
import sys
from typing import Any, Dict, List, Optional, Set, Tuple

import click
import structlog
from structlog.stdlib import LoggerFactory
from evergreen.api import EvergreenApi, RetryingEvergreenApi
from git import Repo
from shrub.config import Configuration
from shrub.variant import DisplayTaskDefinition

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
import buildscripts.resmokelib.parser
import buildscripts.util.read_config as read_config
from buildscripts.burn_in_tests import create_task_list_for_tests, is_file_a_test_file
from buildscripts.ciconfig.evergreen import (
    EvergreenProjectConfig,
    ResmokeArgs,
    parse_evergreen_file,
)
from buildscripts.evergreen_generate_resmoke_tasks import (
    CONFIG_FORMAT_FN,
    DEFAULT_CONFIG_VALUES,
    REQUIRED_CONFIG_KEYS,
    ConfigOptions,
    GenerateSubSuites,
    remove_gen_suffix,
    write_file_dict,
)
from buildscripts.patch_builds.change_data import find_changed_files
from buildscripts.patch_builds.selected_tests_service import SelectedTestsService

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.getLogger(__name__)

EVERGREEN_FILE = "etc/evergreen.yml"
EVG_CONFIG_FILE = ".evergreen.yml"
EXTERNAL_LOGGERS = {
    "evergreen",
    "git",
    "urllib3",
}
SELECTED_TESTS_CONFIG_DIR = "generated_resmoke_config"
THRESHOLD_FOR_RELATED_TESTS = 0.1


class SelectedTestsConfigOptions(ConfigOptions):
    """Retrieve configuration from a config file."""

    @classmethod
    # pylint: disable=too-many-arguments,W0221
    def from_file(cls, filepath: str, overwrites: Dict[str, Any], required_keys: Set[str],
                  defaults: Dict[str, Any], formats: Dict[str, type]):
        """
        Create an instance of SelectedTestsConfigOptions based on the given config file.

        :param filepath: Path to file containing configuration.
        :param overwrites: Dict of configuration values to overwrite those listed in filepath.
        :param required_keys: Set of keys required by this config.
        :param defaults: Dict of default values for keys.
        :param formats: Dict with functions to format values before returning.
        :return: Instance of SelectedTestsConfigOptions.
        """
        config_from_file = read_config.read_config_file(filepath)
        return cls({**config_from_file, **overwrites}, required_keys, defaults, formats)

    @property
    def run_tests_task(self):
        """Return name of task name for s3 folder containing generated tasks config."""
        return remove_gen_suffix(self.name_of_generating_task)

    @property
    def run_tests_build_variant(self):
        """Return name of build_variant for s3 folder containing generated tasks config."""
        return self.name_of_generating_build_variant

    @property
    def run_tests_build_id(self):
        """Return name of build_id for s3 folder containing generated tasks config."""
        return self.name_of_generating_build_id

    @property
    def create_misc_suite(self):
        """Whether or not a _misc suite file should be created."""
        return False

    def generate_display_task(self, task_names: List[str]):
        """
        Generate a display task with execution tasks.

        :param task_names: The names of the execution tasks to include under the display task.
        """
        return DisplayTaskDefinition(f"{self.task}_{self.variant}").execution_tasks(task_names)


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


def _find_related_test_files(
        selected_tests_service: SelectedTestsService,
        changed_files: Set[str],
) -> Set[str]:
    """
    Request related test files from selected-tests service.

    :param selected_tests_service: Selected-tests service.
    :param changed_files: Set of changed_files.
    return: Set of test files returned by selected-tests service that are valid test files.
    """
    test_mappings = selected_tests_service.get_test_mappings(THRESHOLD_FOR_RELATED_TESTS,
                                                             changed_files)
    return {
        test_file["name"]
        for test_mapping in test_mappings for test_file in test_mapping["test_files"]
        if is_file_a_test_file(test_file["name"])
    }


def _get_selected_tests_task_configuration(expansion_file):
    """
    Look up task config of the selected tests task.

    :param expansion_file: Configuration file.
    return: Task configuration values.
    """
    expansions = read_config.read_config_file(expansion_file)
    return {
        "name_of_generating_task": expansions["task_name"],
        "name_of_generating_build_variant": expansions["build_variant"],
        "name_of_generating_build_id": expansions["build_id"]
    }


def _get_evg_task_configuration(
        evg_conf: EvergreenProjectConfig,
        build_variant: str,
        task_name: str,
        test_list_info: dict,
):
    """
    Look up task config of the task to be generated.

    :param evg_conf: Evergreen configuration.
    :param build_variant: Build variant to collect task info from.
    :param task_name: Name of task to get info for.
    :param test_list_info: The value for a given task_name in the tests_by_task dict.
    return: Task configuration values.
    """
    evg_build_variant = evg_conf.get_variant(build_variant)
    task = evg_build_variant.get_task(task_name)
    if task.is_generate_resmoke_task:
        task_vars = task.generate_resmoke_tasks_command["vars"]
    else:
        task_vars = task.run_tests_command["vars"]
        task_vars.update({"fallback_num_sub_suites": "1"})

    suite_name = ResmokeArgs.get_arg(task_vars["resmoke_args"], "suites")
    if suite_name:
        task_vars.update({"suite": suite_name})

    resmoke_args_without_suites = ResmokeArgs.remove_arg(task_vars["resmoke_args"], "suites")
    task_vars["resmoke_args"] = resmoke_args_without_suites

    return {
        "task_name": task_name, "build_variant": build_variant,
        "selected_tests_to_run": set(test_list_info["tests"]), **task_vars
    }


def _generate_shrub_config(evg_api: EvergreenApi, evg_conf: EvergreenProjectConfig,
                           expansion_file: str, tests_by_task: dict, build_variant: str):
    """
    Generate a dict containing file names and contents for the generated configs.

    :param evg_api: Evergreen API object.
    :param evg_conf: Evergreen configuration.
    :param expansion_file: Configuration file.
    :param tests_by_task: Dictionary of tests and tasks to run.
    :param build_variant: Build variant to collect task info from.
    return: Dict of files and file contents for generated tasks.
    """
    shrub_config = Configuration()
    shrub_task_config = None
    config_dict_of_generated_tasks = {}
    for task_name, test_list_info in tests_by_task.items():
        evg_task_config = _get_evg_task_configuration(evg_conf, build_variant, task_name,
                                                      test_list_info)
        selected_tests_task_config = _get_selected_tests_task_configuration(expansion_file)
        evg_task_config.update(selected_tests_task_config)
        LOGGER.debug("Calculated overwrite_values", overwrite_values=evg_task_config)
        config_options = SelectedTestsConfigOptions.from_file(
            expansion_file,
            evg_task_config,
            REQUIRED_CONFIG_KEYS,
            DEFAULT_CONFIG_VALUES,
            CONFIG_FORMAT_FN,
        )
        suite_files_dict, shrub_task_config = GenerateSubSuites(
            evg_api, config_options).generate_task_config_and_suites(shrub_config)
        config_dict_of_generated_tasks.update(suite_files_dict)
    if shrub_task_config:
        config_dict_of_generated_tasks["selected_tests_config.json"] = shrub_task_config
    return config_dict_of_generated_tasks


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
    "--build-variant",
    "build_variant",
    required=True,
    help="Tasks to run will be selected from this build variant.",
)
@click.option(
    "--selected-tests-config",
    "selected_tests_config",
    required=True,
    metavar="FILE",
    help="Configuration file with connection info for selected tests service.",
)
# pylint: disable=too-many-arguments
def main(
        verbose: bool,
        expansion_file: str,
        evg_api_config: str,
        build_variant: str,
        selected_tests_config: str,
):
    """
    Select tasks to be run based on changed files in a patch build.

    :param verbose: Log extra debug information.
    :param expansion_file: Configuration file.
    :param evg_api_config: Location of configuration file to connect to evergreen.
    :param build_variant: Build variant to query tasks from.
    :param selected_tests_config: Location of config file to connect to elected-tests service.
    """
    _configure_logging(verbose)

    evg_api = RetryingEvergreenApi.get_api(config_file=evg_api_config)
    evg_conf = parse_evergreen_file(EVERGREEN_FILE)
    selected_tests_service = SelectedTestsService.from_file(selected_tests_config)

    repo = Repo(".")
    changed_files = find_changed_files(repo)
    buildscripts.resmokelib.parser.set_options()
    LOGGER.debug("Found changed files", files=changed_files)
    related_test_files = _find_related_test_files(selected_tests_service, changed_files)
    LOGGER.debug("related test files found", related_test_files=related_test_files)
    if related_test_files:
        tests_by_task = create_task_list_for_tests(related_test_files, build_variant, evg_conf)
        LOGGER.debug("tests and tasks found", tests_by_task=tests_by_task)
        config_dict_of_generated_tasks = _generate_shrub_config(evg_api, evg_conf, expansion_file,
                                                                tests_by_task, build_variant)

        write_file_dict(SELECTED_TESTS_CONFIG_DIR, config_dict_of_generated_tasks)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
