#!/usr/bin/env python3
"""
Resmoke Test Suite Generator.

Analyze the evergreen history for tests run under the given task and create new evergreen tasks
to attempt to keep the task runtime under a specified amount.
"""
from datetime import timedelta, datetime
import os
import sys
from typing import Optional

import click
import inject
import structlog

from pydantic.main import BaseModel
from evergreen.api import EvergreenApi, RetryingEvergreenApi

# Get relative imports to work when the package is not installed on the PYTHONPATH.
from buildscripts.task_generation.gen_task_validation import GenTaskValidationService
from buildscripts.util.taskname import remove_gen_suffix

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.task_generation.evg_config_builder import EvgConfigBuilder
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.gen_task_service import GenTaskOptions, ResmokeGenTaskParams
from buildscripts.task_generation.suite_split_strategies import SplitStrategy, FallbackStrategy, \
    greedy_division, round_robin_fallback
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyConfig
from buildscripts.task_generation.suite_split import SuiteSplitConfig, SuiteSplitParameters
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.fileops import read_yaml_file
# pylint: enable=wrong-import-position

LOGGER = structlog.getLogger(__name__)

DEFAULT_TEST_SUITE_DIR = os.path.join("buildscripts", "resmokeconfig", "suites")
EVG_CONFIG_FILE = "./.evergreen.yml"
GENERATE_CONFIG_FILE = "etc/generate_subtasks_config.yml"
LOOKBACK_DURATION_DAYS = 14
GEN_SUFFIX = "_gen"
GEN_PARENT_TASK = "generator_tasks"
GENERATED_CONFIG_DIR = "generated_resmoke_config"
ASAN_SIGNATURE = "detect_leaks=1"

DEFAULT_MAX_SUB_SUITES = 5
DEFAULT_MAX_TESTS_PER_SUITE = 100
DEFAULT_TARGET_RESMOKE_TIME = 60


class EvgExpansions(BaseModel):
    """
    Evergreen expansions file contents.

    build_id: ID of build being run.
    build_variant: Build variant task is being generated under.
    is_patch: Is this part of a patch build.
    large_distro_name: Name of distro to use for 'large' tasks.
    max_sub_suites: Max number of sub-suites to create for a single task.
    max_tests_per_suite: Max number of tests to include in a single sub-suite.
    project: Evergreen project being run in.
    resmoke_args: Arguments to pass to resmoke for generated tests.
    resmoke_jobs_max: Max number of jobs for resmoke to run in parallel.
    resmoke_repeat_suites: Number of times resmoke should repeat each suite.
    revision: Git revision being run against.
    san_options: SAN options build variant is running under.
    suite: Name of test suite being generated.
    target_resmoke_time: Target time (in minutes) to keep sub-suite under.
    task_id: ID of task creating the generated configuration.
    task_name: Name of task creating the generated configuration.
    use_large_distro: Should the generated tasks run on "large" distros.
    require_multiversion: Requires downloading Multiversion binaries.
    """

    build_id: str
    build_variant: str
    is_patch: Optional[bool]
    large_distro_name: Optional[str]
    max_sub_suites: int = DEFAULT_MAX_SUB_SUITES
    max_tests_per_suite: int = DEFAULT_MAX_TESTS_PER_SUITE
    project: str
    resmoke_args: str = ""
    resmoke_jobs_max: Optional[int]
    resmoke_repeat_suites: int = 1
    revision: str
    san_options: Optional[str]
    suite: Optional[str]
    target_resmoke_time: int = DEFAULT_TARGET_RESMOKE_TIME
    task_id: str
    task_name: str
    use_large_distro: bool = False
    require_multiversion: Optional[bool]

    @classmethod
    def from_yaml_file(cls, path: str) -> "EvgExpansions":
        """Read the generation configuration from the given file."""
        return cls(**read_yaml_file(path))

    @property
    def task(self) -> str:
        """Get the task being generated."""
        return remove_gen_suffix(self.task_name)

    def is_asan_build(self) -> bool:
        """Determine if this task is an ASAN build."""
        san_options = self.san_options
        if san_options:
            return ASAN_SIGNATURE in san_options
        return False

    def get_suite_split_config(self, start_date: datetime, end_date: datetime) -> SuiteSplitConfig:
        """
        Get the configuration for splitting suites based on Evergreen expansions.

        :param start_date: Start date for historic stats lookup.
        :param end_date: End date for historic stats lookup.
        :return: Configuration to use for splitting suites.
        """
        return SuiteSplitConfig(
            evg_project=self.project,
            target_resmoke_time=self.target_resmoke_time,
            max_sub_suites=self.max_sub_suites,
            max_tests_per_suite=self.max_tests_per_suite,
            start_date=start_date,
            end_date=end_date,
        )

    def get_evg_config_gen_options(self, generated_config_dir: str) -> GenTaskOptions:
        """
        Get the configuration for generating tasks from Evergreen expansions.

        :param generated_config_dir: Directory to write generated configuration.
        :return: Configuration to use for splitting suites.
        """
        return GenTaskOptions(
            create_misc_suite=True,
            is_patch=self.is_patch,
            generated_config_dir=generated_config_dir,
            use_default_timeouts=False,
        )

    def get_suite_split_params(self) -> SuiteSplitParameters:
        """Get the parameters to use for splitting suites."""
        task = remove_gen_suffix(self.task_name)
        return SuiteSplitParameters(
            build_variant=self.build_variant,
            task_name=task,
            suite_name=self.suite or task,
            filename=self.suite or task,
            test_file_filter=None,
            is_asan=self.is_asan_build(),
        )

    def get_gen_params(self) -> "ResmokeGenTaskParams":
        """Get the parameters to use for generating tasks."""
        return ResmokeGenTaskParams(
            use_large_distro=self.use_large_distro, large_distro_name=self.large_distro_name,
            require_multiversion=self.require_multiversion,
            repeat_suites=self.resmoke_repeat_suites, resmoke_args=self.resmoke_args,
            resmoke_jobs_max=self.resmoke_jobs_max, config_location=
            f"{self.build_variant}/{self.revision}/generate_tasks/{self.task}_gen-{self.build_id}.tgz"
        )


class EvgGenResmokeTaskOrchestrator:
    """Orchestrator for generating an resmoke tasks."""

    @inject.autoparams()
    def __init__(self, gen_task_validation: GenTaskValidationService,
                 gen_task_options: GenTaskOptions) -> None:
        """
        Initialize the orchestrator.

        :param gen_task_validation: Generate tasks validation service.
        :param gen_task_options: Options for how tasks are generated.
        """
        self.gen_task_validation = gen_task_validation
        self.gen_task_options = gen_task_options

    def generate_task(self, task_id: str, split_params: SuiteSplitParameters,
                      gen_params: ResmokeGenTaskParams) -> None:
        """
        Generate the specified resmoke task.

        :param task_id: Task ID of generating task.
        :param split_params: Parameters describing how the task should be split.
        :param gen_params: Parameters describing how the task should be generated.
        """
        LOGGER.debug("config options", split_params=split_params, gen_params=gen_params)
        if not self.gen_task_validation.should_task_be_generated(task_id):
            LOGGER.info("Not generating configuration due to previous successful generation.")
            return

        builder = EvgConfigBuilder()  # pylint: disable=no-value-for-parameter

        builder.generate_suite(split_params, gen_params)
        builder.add_display_task(GEN_PARENT_TASK, {f"{split_params.task_name}{GEN_SUFFIX}"},
                                 split_params.build_variant)
        generated_config = builder.build(split_params.task_name + ".json")
        generated_config.write_all_to_dir(self.gen_task_options.generated_config_dir)


@click.command()
@click.option("--expansion-file", type=str, required=True,
              help="Location of expansions file generated by evergreen.")
@click.option("--evergreen-config", type=str, default=EVG_CONFIG_FILE,
              help="Location of evergreen configuration file.")
@click.option("--verbose", is_flag=True, default=False, help="Enable verbose logging.")
def main(expansion_file: str, evergreen_config: str, verbose: bool) -> None:
    """
    Create a configuration for generate tasks to create sub suites for the specified resmoke suite.

    The `--expansion-file` should contain all the configuration needed to generate the tasks.
    \f
    :param expansion_file: Configuration file.
    :param evergreen_config: Evergreen configuration file.
    :param verbose: Use verbose logging.
    """
    enable_logging(verbose)

    end_date = datetime.utcnow().replace(microsecond=0)
    start_date = end_date - timedelta(days=LOOKBACK_DURATION_DAYS)

    evg_expansions = EvgExpansions.from_yaml_file(expansion_file)

    def dependencies(binder: inject.Binder) -> None:
        binder.bind(SuiteSplitConfig, evg_expansions.get_suite_split_config(start_date, end_date))
        binder.bind(SplitStrategy, greedy_division)
        binder.bind(FallbackStrategy, round_robin_fallback)
        binder.bind(GenTaskOptions, evg_expansions.get_evg_config_gen_options(GENERATED_CONFIG_DIR))
        binder.bind(EvergreenApi, RetryingEvergreenApi.get_api(config_file=evergreen_config))
        binder.bind(GenerationConfiguration,
                    GenerationConfiguration.from_yaml_file(GENERATE_CONFIG_FILE))
        binder.bind(ResmokeProxyConfig,
                    ResmokeProxyConfig(resmoke_suite_dir=DEFAULT_TEST_SUITE_DIR))

    inject.configure(dependencies)

    gen_task_orchestrator = EvgGenResmokeTaskOrchestrator()  # pylint: disable=no-value-for-parameter
    gen_task_orchestrator.generate_task(evg_expansions.task_id,
                                        evg_expansions.get_suite_split_params(),
                                        evg_expansions.get_gen_params())


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
