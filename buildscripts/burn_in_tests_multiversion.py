#!/usr/bin/env python3
"""Command line utility for running newly added or modified jstests under the appropriate multiversion passthrough suites."""

import os
from datetime import datetime
from functools import partial
from typing import List, Dict, NamedTuple

import click
import inject
from git import Repo
import structlog
from structlog.stdlib import LoggerFactory
from evergreen.api import EvergreenApi, RetryingEvergreenApi

from buildscripts.burn_in_tests import EVERGREEN_FILE, \
    DEFAULT_REPO_LOCATIONS, create_tests_by_task, TaskInfo
from buildscripts.ciconfig.evergreen import parse_evergreen_file, EvergreenProjectConfig
from buildscripts.evergreen_burn_in_tests import GenerateConfig, DEFAULT_PROJECT, CONFIG_FILE, \
    EvergreenFileChangeDetector
from buildscripts.resmokelib.suitesconfig import get_named_suites_with_root_level_key
from buildscripts.task_generation.evg_config_builder import EvgConfigBuilder
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.generated_config import GeneratedConfiguration
from buildscripts.task_generation.multiversion_util import MultiversionUtilService
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyConfig
from buildscripts.task_generation.suite_split import SuiteSplitConfig, SuiteSplitParameters
from buildscripts.task_generation.suite_split_strategies import SplitStrategy, greedy_division, \
    FallbackStrategy, round_robin_fallback
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions
from buildscripts.task_generation.task_types.multiversion_tasks import MultiversionGenTaskParams
from buildscripts.util.cmdutils import enable_logging

structlog.configure(logger_factory=LoggerFactory())
LOGGER = structlog.getLogger(__name__)

MULTIVERSION_CONFIG_KEY = "use_in_multiversion"
MULTIVERSION_PASSTHROUGH_TAG = "multiversion_passthrough"
BURN_IN_MULTIVERSION_TASK = "burn_in_tests_multiversion"
DEFAULT_CONFIG_DIR = "generated_resmoke_config"
DEFAULT_TEST_SUITE_DIR = os.path.join("buildscripts", "resmokeconfig", "suites")


def filter_list(item: str, input_list: List[str]) -> bool:
    """
    Filter to determine if the given item is in the given list.

    :param item: Item to search for.
    :param input_list: List to search.
    :return: True if the item is contained in the list.
    """
    return item in input_list


class BurnInConfig(NamedTuple):
    """Configuration for generating build in."""

    build_id: str
    build_variant: str
    revision: str

    def build_config_location(self) -> str:
        """Build the configuration location for the generated configuration."""
        return f"{self.build_variant}/{self.revision}/generate_tasks/burn_in_tests_multiversion_gen_config-{self.build_id}.tgz"


class MultiversionBurnInOrchestrator:
    """Orchestrator for generating multiversion burn_in_tests."""

    @inject.autoparams()
    def __init__(self, change_detector: EvergreenFileChangeDetector,
                 evg_conf: EvergreenProjectConfig, multiversion_util: MultiversionUtilService,
                 burn_in_config: BurnInConfig) -> None:
        """
        Initialize the orchestrator.

        :param change_detector: Service to find changed files.
        :param evg_conf: Evergreen project configuration.
        :param multiversion_util: Multiversion utilities.
        :param burn_in_config: Configuration for generating burn in.
        """
        self.change_detector = change_detector
        self.evg_config = evg_conf
        self.multiversion_util = multiversion_util
        self.burn_in_config = burn_in_config

    def validate_multiversion_tasks_and_suites(self) -> None:
        """
        Validate that the multiversion suites and tasks match up.

        We expect the number of suites with MULTIVERSION_PASSTHROUGH_TAG to be the same as in
        multiversion_suites. Multiversion passthrough suites must include
        MULTIVERSION_CONFIG_KEY as a root level key and must be set to true.

        Throws an exception if there are inconsistencies.
        """
        multiversion_tasks = self.evg_config.get_task_names_by_tag(MULTIVERSION_PASSTHROUGH_TAG)
        LOGGER.debug("Multiversion tasks by tag", tasks=multiversion_tasks,
                     tag=MULTIVERSION_PASSTHROUGH_TAG)

        multiversion_suites = get_named_suites_with_root_level_key(MULTIVERSION_CONFIG_KEY)
        assert len(multiversion_tasks) == len(multiversion_suites)

    def generate_tests(self, repos: List[Repo], generate_config: GenerateConfig,
                       target_file: str) -> None:
        """
        Generate evergreen configuration to run any changed tests and save them to disk.

        :param repos: List of repos to check for changed tests.
        :param generate_config: Configuration for how to generate tasks.
        :param target_file: File to write configuration to.
        """
        tests_by_task = self.find_changes(repos, generate_config)
        generated_config = self.generate_configuration(tests_by_task, target_file,
                                                       generate_config.build_variant)
        generated_config.write_all_to_dir(DEFAULT_CONFIG_DIR)

    def find_changes(self, repos: List[Repo],
                     generate_config: GenerateConfig) -> Dict[str, TaskInfo]:
        """
        Find tests and tasks to run based on test changes.

        :param repos: List of repos to check for changed tests.
        :param generate_config: Configuration for how to generate tasks.
        :return: Dictionary of tasks with tests to run in them.
        """
        changed_tests = self.change_detector.find_changed_tests(repos)
        tests_by_task = create_tests_by_task(generate_config.build_variant, self.evg_config,
                                             changed_tests)
        LOGGER.debug("tests and tasks found", tests_by_task=tests_by_task)
        return tests_by_task

    # pylint: disable=too-many-locals
    def generate_configuration(self, tests_by_task: Dict[str, TaskInfo], target_file: str,
                               build_variant: str) -> GeneratedConfiguration:
        """
        Generate configuration for the given tasks and tests.

        :param tests_by_task: Map of what to generate.
        :param target_file: Location to write generated configuration.
        :param build_variant: Name of build variant being generated on.
        :return: Generated configuration to create requested tasks and tests.
        """
        builder = EvgConfigBuilder()  # pylint: disable=no-value-for-parameter
        build_variant_config = self.evg_config.get_variant(build_variant)
        is_asan = build_variant_config.is_asan_build()
        tasks = set()
        if tests_by_task:
            # Get the multiversion suites that will run in as part of burn_in_multiversion.
            multiversion_suites = get_named_suites_with_root_level_key(MULTIVERSION_CONFIG_KEY)
            for suite in multiversion_suites:
                task_name = suite["origin"]
                if task_name not in tests_by_task.keys():
                    # Only generate burn in multiversion tasks for suites that would run the
                    # detected changed tests.
                    continue

                LOGGER.debug("Generating multiversion suite", suite=suite["multiversion_name"])
                test_list = tests_by_task[task_name].tests
                split_params = SuiteSplitParameters(
                    task_name=suite["multiversion_name"], suite_name=task_name, filename=task_name,
                    test_file_filter=partial(filter_list, input_list=test_list),
                    build_variant=build_variant, is_asan=is_asan)
                version_configs = self.multiversion_util.get_version_configs_for_suite(task_name)
                gen_params = MultiversionGenTaskParams(
                    mixed_version_configs=version_configs,
                    is_sharded=self.multiversion_util.is_suite_sharded(task_name),
                    resmoke_args="",
                    parent_task_name="burn_in_tests_multiversion",
                    origin_suite=task_name,
                    use_large_distro=False,
                    large_distro_name=None,
                    name_prefix="burn_in_multiversion",
                    create_misc_suite=False,
                    add_to_display_task=False,
                    config_location=self.burn_in_config.build_config_location(),
                )

                tasks = tasks.union(builder.add_multiversion_burn_in_test(split_params, gen_params))

        if len(tasks) == 0:
            builder.get_build_variant(build_variant)

        executions_tasks = {task.name for task in tasks}
        executions_tasks.add("burn_in_tests_multiversion_gen")
        builder.add_display_task(display_task_name="burn_in_multiversion",
                                 execution_task_names=executions_tasks, build_variant=build_variant)

        return builder.build(target_file)


@click.command()
@click.option("--generate-tasks-file", "generate_tasks_file", required=True, metavar='FILE',
              help="Run in 'generate.tasks' mode. Store task config to given file.")
@click.option("--build-variant", "build_variant", default=None, metavar='BUILD_VARIANT',
              help="Tasks to run will be selected from this build variant.")
@click.option("--run-build-variant", "run_build_variant", default=None, metavar='BUILD_VARIANT',
              help="Burn in tasks will be generated on this build variant.")
@click.option("--distro", "distro", default=None, metavar='DISTRO',
              help="The distro the tasks will execute on.")
@click.option("--project", "project", default=DEFAULT_PROJECT, metavar='PROJECT',
              help="The evergreen project the tasks will execute on.")
@click.option("--evg-api-config", "evg_api_config", default=CONFIG_FILE, metavar="FILE",
              help="Configuration file with connection info for Evergreen API.")
@click.option("--revision", required=True, help="Revision generation is being run against.")
@click.option("--build-id", required=True, help="ID of build being run against.")
@click.option("--verbose", "verbose", default=False, is_flag=True, help="Enable extra logging.")
@click.option("--task_id", "task_id", default=None, metavar='TASK_ID',
              help="The evergreen task id.")
# pylint: disable=too-many-arguments,too-many-locals
def main(build_variant, run_build_variant, distro, project, generate_tasks_file, evg_api_config,
         verbose, task_id, revision, build_id):
    """
    Run new or changed tests in repeated mode to validate their stability.

    Running burn_in_tests_multiversion will run new or changed tests against the appropriate generated multiversion
    suites. The purpose of these tests are to signal bugs in the generated multiversion suites as these tasks are
    excluded from the required build variants and are only run in certain daily build variants. As such, we only expect
    the burn-in multiversion tests to be run once for each binary version configuration, and `--repeat-*` arguments
    should be None when executing this script.

    There are two modes that burn_in_tests_multiversion can run in:

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
    :param evg_api_config: Location of configuration file to connect to evergreen.
    :param verbose: Log extra debug information.
    """
    enable_logging(verbose)

    burn_in_config = BurnInConfig(build_variant=build_variant, build_id=build_id, revision=revision)
    evg_conf = parse_evergreen_file(EVERGREEN_FILE)
    generate_config = GenerateConfig(build_variant=build_variant,
                                     run_build_variant=run_build_variant,
                                     distro=distro,
                                     project=project,
                                     task_id=task_id)  # yapf: disable
    generate_config.validate(evg_conf)

    gen_task_options = GenTaskOptions(
        create_misc_suite=False,
        is_patch=True,
        generated_config_dir=DEFAULT_CONFIG_DIR,
        use_default_timeouts=False,
    )
    split_task_options = SuiteSplitConfig(
        evg_project=project,
        target_resmoke_time=60,
        max_sub_suites=100,
        max_tests_per_suite=1,
        start_date=datetime.utcnow(),
        end_date=datetime.utcnow(),
        default_to_fallback=True,
    )

    repos = [Repo(x) for x in DEFAULT_REPO_LOCATIONS if os.path.isdir(x)]

    def dependencies(binder: inject.Binder) -> None:
        evg_api = RetryingEvergreenApi.get_api(config_file=evg_api_config)
        binder.bind(SuiteSplitConfig, split_task_options)
        binder.bind(SplitStrategy, greedy_division)
        binder.bind(FallbackStrategy, round_robin_fallback)
        binder.bind(EvergreenProjectConfig, evg_conf)
        binder.bind(GenTaskOptions, gen_task_options)
        binder.bind(EvergreenApi, evg_api)
        binder.bind(GenerationConfiguration, GenerationConfiguration.from_yaml_file())
        binder.bind(ResmokeProxyConfig,
                    ResmokeProxyConfig(resmoke_suite_dir=DEFAULT_TEST_SUITE_DIR))
        binder.bind(EvergreenFileChangeDetector, EvergreenFileChangeDetector(task_id, evg_api))
        binder.bind(BurnInConfig, burn_in_config)

    inject.configure(dependencies)

    burn_in_orchestrator = MultiversionBurnInOrchestrator()  # pylint: disable=no-value-for-parameter
    burn_in_orchestrator.validate_multiversion_tasks_and_suites()
    burn_in_orchestrator.generate_tests(repos, generate_config, generate_tasks_file)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
