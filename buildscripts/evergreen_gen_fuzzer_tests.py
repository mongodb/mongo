#!/usr/bin/env python3
"""Generate fuzzer tests to run in evergreen in parallel."""
import os
from typing import Optional

import click
import inject
from pydantic import BaseModel
from evergreen import EvergreenApi, RetryingEvergreenApi

from buildscripts.task_generation.evg_config_builder import EvgConfigBuilder
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.gen_task_service import GenTaskOptions, FuzzerGenTaskParams
from buildscripts.task_generation.gen_task_validation import GenTaskValidationService
from buildscripts.task_generation.generated_config import GeneratedConfiguration
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyConfig
from buildscripts.task_generation.suite_split import SuiteSplitService
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.fileops import read_yaml_file

CONFIG_DIRECTORY = "generated_resmoke_config"
GEN_PARENT_TASK = "generator_tasks"
GENERATE_CONFIG_FILE = "etc/generate_subtasks_config.yml"
DEFAULT_TEST_SUITE_DIR = os.path.join("buildscripts", "resmokeconfig", "suites")
EVG_CONFIG_FILE = "./.evergreen.yml"


class EvgExpansions(BaseModel):
    """
    Evergreen expansions to read for configuration.

    build_id: ID of build being run.
    build_variant: Build Variant being run on.
    continue_on_failure: Should tests continue after encountering a failure.
    is_patch: Are tests being run in a patch build.
    jstestfuzz_vars: Variable to pass to jstestfuzz command.
    large_distro_name: Name of "large" distro to use.
    name: Name of task to generate.
    npm_command: NPM command to generate fuzzer tests.
    num_files: Number of fuzzer files to generate.
    num_tasks: Number of sub-tasks to generate.
    resmoke_args: Arguments to pass to resmoke.
    resmoke_jobs_max: Max number of jobs resmoke should execute in parallel.
    revision: git revision being run against.
    should_shuffle: Should remove shuffle tests before executing.
    suite: Resmoke suite to run the tests.
    task_id: ID of task currently being executed.
    require_multiversion: Requires downloading Multiversion binaries.
    timeout_secs: Timeout to set for task execution.
    use_large_distro: Should tasks be generated to run on a large distro.
    """

    build_id: str
    build_variant: str
    continue_on_failure: bool
    is_patch: Optional[bool]
    jstestfuzz_vars: Optional[str]
    large_distro_name: Optional[str]
    name: str
    npm_command: Optional[str]
    num_files: int
    num_tasks: int
    resmoke_args: str
    resmoke_jobs_max: int
    revision: str
    should_shuffle: bool
    suite: str
    task_id: str
    timeout_secs: int
    use_large_distro: Optional[bool]
    require_multiversion: Optional[bool]

    @classmethod
    def from_yaml_file(cls, path: str) -> "EvgExpansions":
        """
        Read the generation configuration from the given file.

        :param path: Path to file.
        :return: Parse evergreen expansions.
        """
        return cls(**read_yaml_file(path))

    def gen_task_options(self) -> GenTaskOptions:
        """Determine the options for generating tasks based on the given expansions."""
        return GenTaskOptions(
            is_patch=self.is_patch,
            create_misc_suite=True,
            generated_config_dir=CONFIG_DIRECTORY,
            use_default_timeouts=False,
        )

    def fuzzer_gen_task_params(self) -> FuzzerGenTaskParams:
        """Determine the parameters for generating fuzzer tasks based on the given expansions."""
        return FuzzerGenTaskParams(
            task_name=self.name, num_files=self.num_files, num_tasks=self.num_tasks,
            resmoke_args=self.resmoke_args, npm_command=self.npm_command or "jstestfuzz",
            jstestfuzz_vars=self.jstestfuzz_vars, variant=self.build_variant,
            continue_on_failure=self.continue_on_failure, resmoke_jobs_max=self.resmoke_jobs_max,
            should_shuffle=self.should_shuffle, timeout_secs=self.timeout_secs,
            require_multiversion=self.require_multiversion, suite=self.suite,
            use_large_distro=self.use_large_distro, large_distro_name=self.large_distro_name,
            config_location=
            f"{self.build_variant}/{self.revision}/generate_tasks/{self.name}_gen-{self.build_id}.tgz"
        )


class EvgGenFuzzerOrchestrator:
    """Orchestrate the generation of fuzzer tasks."""

    @inject.autoparams()
    def __init__(self, validation_service: GenTaskValidationService) -> None:
        """
        Initialize the orchestrator.

        :param validation_service: Validation Service for generating tasks.
        """
        self.validation_service = validation_service

    @staticmethod
    def generate_config(fuzzer_params: FuzzerGenTaskParams) -> GeneratedConfiguration:
        """
        Generate a fuzzer task based on the given parameters.

        :param fuzzer_params: Parameters describing how fuzzer should be generated.
        :return: Configuration to generate the specified fuzzer.
        """
        builder = EvgConfigBuilder()  # pylint: disable=no-value-for-parameter

        builder.generate_fuzzer(fuzzer_params)
        builder.add_display_task(GEN_PARENT_TASK, {f"{fuzzer_params.task_name}_gen"},
                                 fuzzer_params.variant)
        return builder.build(fuzzer_params.task_name + ".json")

    def generate_fuzzer(self, task_id: str, fuzzer_params: FuzzerGenTaskParams) -> None:
        """
        Save the configuration to generate the specified fuzzer to disk.

        :param task_id: ID of task doing the generation.
        :param fuzzer_params: Parameters describing how fuzzer should be generated.
        """
        if not self.validation_service.should_task_be_generated(task_id):
            print("Not generating configuration due to previous successful generation.")
            return

        generated_config = self.generate_config(fuzzer_params)
        generated_config.write_all_to_dir(CONFIG_DIRECTORY)


@click.command()
@click.option("--expansion-file", type=str, required=True,
              help="Location of expansions file generated by evergreen.")
@click.option("--evergreen-config", type=str, default=EVG_CONFIG_FILE,
              help="Location of evergreen configuration file.")
@click.option("--verbose", is_flag=True, default=False, help="Enable verbose logging.")
def main(expansion_file: str, evergreen_config: str, verbose: bool) -> None:
    """Generate fuzzer tests to run in evergreen."""
    enable_logging(verbose)

    evg_expansions = EvgExpansions.from_yaml_file(expansion_file)

    def dependencies(binder: inject.Binder) -> None:
        binder.bind(SuiteSplitService, None)
        binder.bind(GenTaskOptions, evg_expansions.gen_task_options())
        binder.bind(EvergreenApi, RetryingEvergreenApi.get_api(config_file=evergreen_config))
        binder.bind(GenerationConfiguration,
                    GenerationConfiguration.from_yaml_file(GENERATE_CONFIG_FILE))
        binder.bind(ResmokeProxyConfig,
                    ResmokeProxyConfig(resmoke_suite_dir=DEFAULT_TEST_SUITE_DIR))

    inject.configure(dependencies)

    gen_fuzzer_orchestrator = EvgGenFuzzerOrchestrator()  # pylint: disable=no-value-for-parameter
    gen_fuzzer_orchestrator.generate_fuzzer(evg_expansions.task_id,
                                            evg_expansions.fuzzer_gen_task_params())


if __name__ == '__main__':
    main()  # pylint: disable=no-value-for-parameter
