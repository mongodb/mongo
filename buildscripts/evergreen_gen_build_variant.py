#!/usr/bin/env python3
"""Generate configuration for a build variant."""
import os
import re
from concurrent.futures import ThreadPoolExecutor as Executor
from datetime import datetime, timedelta
from time import perf_counter
from typing import Optional, Any, List, Set

import click
import inject
import structlog
from pydantic import BaseModel
from evergreen import EvergreenApi, RetryingEvergreenApi
from evergreen import Task as EvgTask

from buildscripts.ciconfig.evergreen import EvergreenProjectConfig, parse_evergreen_file, Task, \
    Variant
from buildscripts.task_generation.evg_config_builder import EvgConfigBuilder
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.gen_task_validation import GenTaskValidationService
from buildscripts.task_generation.multiversion_util import MultiversionUtilService, \
    SHARDED_MIXED_VERSION_CONFIGS, REPL_MIXED_VERSION_CONFIGS
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyConfig
from buildscripts.task_generation.suite_split import SuiteSplitConfig, SuiteSplitParameters
from buildscripts.task_generation.suite_split_strategies import SplitStrategy, FallbackStrategy, \
    greedy_division, round_robin_fallback
from buildscripts.task_generation.task_types.fuzzer_tasks import FuzzerGenTaskParams
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions
from buildscripts.task_generation.task_types.multiversion_tasks import MultiversionGenTaskParams
from buildscripts.task_generation.task_types.resmoke_tasks import ResmokeGenTaskParams
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.fileops import read_yaml_file
from buildscripts.util.taskname import remove_gen_suffix

LOGGER = structlog.get_logger(__name__)

DEFAULT_TEST_SUITE_DIR = os.path.join("buildscripts", "resmokeconfig", "suites")
MAX_WORKERS = 16
LOOKBACK_DURATION_DAYS = 14
MAX_TASK_PRIORITY = 99
GENERATED_CONFIG_DIR = "generated_resmoke_config"
GEN_PARENT_TASK = "generator_tasks"
EXPANSION_RE = re.compile(r"\${(?P<id>[a-zA-Z0-9_]+)(\|(?P<default>.*))?}")


class EvgExpansions(BaseModel):
    """
    Evergreen expansions needed to generate tasks.

    build_id: Build ID being run under.
    build_variant: Build variant being generated.
    gen_task_gran: Granularity of how tasks are being generated.
    is_patch: Whether generation is part of a patch build.
    project: Evergreen project being run under.
    max_test_per_suite: Maximum amount of tests to include in a suite.
    max_sub_suites: Maximum number of sub-suites to generate per task.
    resmoke_repeat_suites: Number of times suites should be repeated.
    revision: Git revision being run against.
    task_name: Name of task running.
    target_resmoke_time: Target time of generated sub-suites.
    task_id: ID of task being run under.
    """

    build_id: str
    build_variant: str
    is_patch: Optional[bool]
    project: str
    max_tests_per_suite: Optional[int] = 100
    max_sub_suites: Optional[int] = 5
    resmoke_repeat_suites: Optional[int] = None
    revision: str
    task_name: str
    target_resmoke_time: Optional[int] = None
    task_id: str

    @classmethod
    def from_yaml_file(cls, path: str) -> "EvgExpansions":
        """
        Read the evergreen expansions from the given YAML file.

        :param path: Path to expansions YAML file.
        :return: Expansions read from file.
        """
        return cls(**read_yaml_file(path))

    def build_suite_split_config(self, start_date: datetime,
                                 end_date: datetime) -> SuiteSplitConfig:
        """
        Get the configuration for splitting suites based on Evergreen expansions.

        :param start_date: Start date for historic stats lookup.
        :param end_date: End date for historic stats lookup.
        :return: Configuration to use for splitting suites.
        """
        return SuiteSplitConfig(
            evg_project=self.project,
            target_resmoke_time=self.target_resmoke_time if self.target_resmoke_time else 60,
            max_sub_suites=self.max_sub_suites,
            max_tests_per_suite=self.max_tests_per_suite,
            start_date=start_date,
            end_date=end_date,
        )

    def build_evg_config_gen_options(self) -> GenTaskOptions:
        """
        Get the configuration for generating tasks from Evergreen expansions.

        :return: Configuration to use for splitting suites.
        """
        return GenTaskOptions(
            create_misc_suite=True,
            is_patch=self.is_patch,
            generated_config_dir=GENERATED_CONFIG_DIR,
            use_default_timeouts=False,
        )

    def config_location(self) -> str:
        """Location where generated configuration is stored."""
        task = remove_gen_suffix(self.task_name)
        return f"{self.build_variant}/{self.revision}/generate_tasks/{task}_gen-{self.build_id}.tgz"


def translate_run_var(run_var: str, build_variant: Variant) -> Any:
    """
    Translate the given "run_var" into an actual value.

    Run_vars can contain evergreen expansions, in which case, the expansion (and possible default
    value) need to be translated into a value we can use.

    :param run_var: Run var to translate.
    :param build_variant: Build variant configuration.
    :return: Value of run_var.
    """
    match = EXPANSION_RE.search(run_var)
    if match:
        value = build_variant.expansion(match.group("id"))
        if value is None:
            value = match.group("default")
        return value
    return run_var


def get_version_configs(is_sharded: bool) -> List[str]:
    """Get the version configurations to use."""
    if is_sharded:
        return SHARDED_MIXED_VERSION_CONFIGS
    return REPL_MIXED_VERSION_CONFIGS


class GenerateBuildVariantOrchestrator:
    """Orchestrator for generating tasks in a build variant."""

    # pylint: disable=too-many-arguments
    @inject.autoparams()
    def __init__(
            self,
            gen_task_validation: GenTaskValidationService,
            gen_task_options: GenTaskOptions,
            evg_project_config: EvergreenProjectConfig,
            evg_expansions: EvgExpansions,
            multiversion_util: MultiversionUtilService,
            evg_api: EvergreenApi,
    ) -> None:
        """
        Initialize the orchestrator.

        :param gen_task_validation: Service to validate task generation.
        :param gen_task_options: Options for how tasks should be generated.
        :param evg_project_config: Configuration for Evergreen Project.
        :param evg_expansions: Evergreen expansions for running task.
        :param multiversion_util: Multiversion utility service.
        :param evg_api: Evergreen API client.
        """
        self.gen_task_validation = gen_task_validation
        self.gen_task_options = gen_task_options
        self.evg_project_config = evg_project_config
        self.evg_expansions = evg_expansions
        self.multiversion_util = multiversion_util
        self.evg_api = evg_api

    def get_build_variant_expansion(self, build_variant_name: str, expansion: str) -> Any:
        """
        Get the value of the given expansion for the specified build variant.

        :param build_variant_name: Build Variant to query.
        :param expansion: Expansion to query.
        :return: Value of given expansion.
        """
        build_variant = self.evg_project_config.get_variant(build_variant_name)
        return build_variant.expansion(expansion)

    def task_def_to_split_params(self, task_def: Task,
                                 build_variant_gen: str) -> SuiteSplitParameters:
        """
        Build parameters for how a task should be split based on its task definition.

        :param task_def: Task definition in evergreen project config.
        :param build_variant_gen: Name of Build Variant being generated.
        :return: Parameters for how task should be split.
        """
        build_variant = self.evg_project_config.get_variant(build_variant_gen)
        task = remove_gen_suffix(task_def.name)
        run_vars = task_def.generate_resmoke_tasks_command.get("vars", {})

        suite = run_vars.get("suite", task)
        return SuiteSplitParameters(
            build_variant=build_variant_gen,
            task_name=task,
            suite_name=suite,
            filename=suite,
            is_asan=build_variant.is_asan_build(),
        )

    def task_def_to_gen_params(self, task_def: Task, build_variant: str) -> ResmokeGenTaskParams:
        """
        Build parameters for how a task should be generated based on its task definition.

        :param task_def: Task definition in evergreen project config.
        :param build_variant: Name of Build Variant being generated.
        :return: Parameters for how task should be generated.
        """
        run_func = task_def.generate_resmoke_tasks_command
        run_vars = run_func["vars"]

        repeat_suites = 1
        if self.evg_expansions.resmoke_repeat_suites:
            repeat_suites = self.evg_expansions.resmoke_repeat_suites

        return ResmokeGenTaskParams(
            use_large_distro=run_vars.get("use_large_distro"),
            require_multiversion=run_vars.get("require_multiversion"),
            repeat_suites=repeat_suites,
            resmoke_args=run_vars.get("resmoke_args"),
            resmoke_jobs_max=run_vars.get("resmoke_jobs_max"),
            large_distro_name=self.get_build_variant_expansion(build_variant, "large_distro_name"),
            config_location=self.evg_expansions.config_location(),
        )

    def task_def_to_mv_gen_params(self, task_def: Task, build_variant: str, is_sharded: bool,
                                  version_config: List[str]) -> MultiversionGenTaskParams:
        """
        Build parameters for how a task should be generated based on its task definition.

        :param task_def: Task definition in evergreen project config.
        :param build_variant: Name of Build Variant being generated.
        :param is_sharded: True if the tasks being generated are for a sharded config.
        :param version_config: List of version configurations to generate.
        :return: Parameters for how task should be generated.
        """
        run_vars = task_def.generate_resmoke_tasks_command["vars"]
        task = remove_gen_suffix(task_def.name)

        return MultiversionGenTaskParams(
            mixed_version_configs=version_config,
            is_sharded=is_sharded,
            resmoke_args=run_vars.get("resmoke_args"),
            parent_task_name=task,
            origin_suite=run_vars.get("suite", task),
            use_large_distro=run_vars.get("use_large_distro"),
            large_distro_name=self.get_build_variant_expansion(build_variant, "large_distro_name"),
            config_location=self.evg_expansions.config_location(),
        )

    def task_def_to_fuzzer_params(
            self, task_def: Task, build_variant: str, is_sharded: Optional[bool] = None,
            version_config: Optional[List[str]] = None) -> FuzzerGenTaskParams:
        """
        Build parameters for how a fuzzer task should be generated based on its task definition.

        :param task_def: Task definition in evergreen project config.
        :param build_variant: Name of Build Variant being generated.
        :param is_sharded: True task if for a sharded configuration.
        :param version_config: List of version configs task is being generated for.
        :return: Parameters for how a fuzzer task should be generated.
        """
        variant = self.evg_project_config.get_variant(build_variant)
        run_vars = task_def.generate_resmoke_tasks_command["vars"]
        run_vars = {k: translate_run_var(v, variant) for k, v in run_vars.items()}

        return FuzzerGenTaskParams(
            task_name=run_vars.get("name"),
            variant=build_variant,
            suite=run_vars.get("suite"),
            num_files=int(run_vars.get("num_files")),
            num_tasks=int(run_vars.get("num_tasks")),
            resmoke_args=run_vars.get("resmoke_args"),
            npm_command=run_vars.get("npm_command", "jstestfuzz"),
            jstestfuzz_vars=run_vars.get("jstestfuzz_vars", ""),
            continue_on_failure=run_vars.get("continue_on_failure"),
            resmoke_jobs_max=run_vars.get("resmoke_jobs_max"),
            should_shuffle=run_vars.get("should_shuffle"),
            timeout_secs=run_vars.get("timeout_secs"),
            require_multiversion=run_vars.get("require_multiversion"),
            use_large_distro=run_vars.get("use_large_distro", False),
            large_distro_name=self.get_build_variant_expansion(build_variant, "large_distro_name"),
            config_location=self.evg_expansions.config_location(),
            is_sharded=is_sharded,
            version_config=version_config,
        )

    def generate(self, task_id: str, build_variant_name: str, output_file: str) -> None:
        """
        Write task configuration for a build variant to disk.

        :param task_id: ID of running task.
        :param build_variant_name: Name of build variant to generate.
        :param output_file: Filename to write generated configuration to.
        """
        if not self.gen_task_validation.should_task_be_generated(task_id):
            LOGGER.info("Not generating configuration due to previous successful generation.")
            return

        builder = EvgConfigBuilder()  # pylint: disable=no-value-for-parameter
        builder = self.generate_build_variant(builder, build_variant_name)

        generated_config = builder.build(output_file)
        generated_config.write_all_to_dir(self.gen_task_options.generated_config_dir)

    # pylint: disable=too-many-locals
    def generate_build_variant(self, builder: EvgConfigBuilder,
                               build_variant_name: str) -> EvgConfigBuilder:
        """
        Generate task configuration for a build variant.

        :param builder: Evergreen configuration builder to use.
        :param build_variant_name: Name of build variant to generate.
        :return: Evergreen configuration builder with build variant configuration.
        """
        LOGGER.info("Generating config", build_variant=build_variant_name)
        start_time = perf_counter()
        task_list = self.evg_project_config.get_variant(build_variant_name).task_names
        tasks_to_hide = set()
        with Executor(max_workers=MAX_WORKERS) as exe:
            jobs = []
            for task_name in task_list:
                task_def = self.evg_project_config.get_task(task_name)
                if task_def.is_generate_resmoke_task:
                    tasks_to_hide.add(task_name)

                    is_sharded = None
                    version_list = None
                    run_vars = task_def.generate_resmoke_tasks_command["vars"]
                    suite = run_vars.get("suite")
                    is_jstestfuzz = run_vars.get("is_jstestfuzz", False)
                    implicit_multiversion = run_vars.get("implicit_multiversion", False)

                    if implicit_multiversion:
                        assert suite is not None
                        is_sharded = self.multiversion_util.is_suite_sharded(suite)
                        version_list = get_version_configs(is_sharded)

                    if is_jstestfuzz:
                        fuzzer_params = self.task_def_to_fuzzer_params(
                            task_def, build_variant_name, is_sharded, version_list)
                        jobs.append(exe.submit(builder.generate_fuzzer, fuzzer_params))
                    else:
                        split_params = self.task_def_to_split_params(task_def, build_variant_name)
                        if implicit_multiversion:
                            gen_params = self.task_def_to_mv_gen_params(
                                task_def, build_variant_name, is_sharded, version_list)
                            jobs.append(
                                exe.submit(builder.add_multiversion_suite, split_params,
                                           gen_params))
                        else:
                            gen_params = self.task_def_to_gen_params(task_def, build_variant_name)
                            jobs.append(
                                exe.submit(builder.generate_suite, split_params, gen_params))

            [j.result() for j in jobs]  # pylint: disable=expression-not-assigned

        end_time = perf_counter()
        duration = end_time - start_time

        LOGGER.info("Finished BV", build_variant=build_variant_name, duration=duration,
                    task_count=len(tasks_to_hide))

        builder.add_display_task(GEN_PARENT_TASK, tasks_to_hide, build_variant_name)
        self.adjust_gen_tasks_priority(tasks_to_hide)
        return builder

    def adjust_task_priority(self, task: EvgTask) -> None:
        """
        Increase the priority of the given task by 1.

        :param task: Task to increase priority of.
        """
        priority = min(task.priority + 1, MAX_TASK_PRIORITY)
        LOGGER.info("Configure task", task_id=task.task_id, priority=priority)
        self.evg_api.configure_task(task.task_id, priority=priority)

    def adjust_gen_tasks_priority(self, gen_tasks: Set[str]) -> int:
        """
        Increase the priority of any "_gen" tasks.

        We want to minimize the time it tasks for the "_gen" tasks to activate the generated
        sub-tasks. We will do that by increase the priority of the "_gen" tasks.

        :param gen_tasks: Set of "_gen" tasks that were found.
        """
        build = self.evg_api.build_by_id(self.evg_expansions.build_id)
        task_list = build.get_tasks()

        with Executor(max_workers=MAX_WORKERS) as exe:
            jobs = [
                exe.submit(self.adjust_task_priority, task) for task in task_list
                if task.display_name in gen_tasks
            ]

        results = [j.result() for j in jobs]
        return len(results)


@click.command()
@click.option("--expansion-file", type=str, required=True,
              help="Location of expansions file generated by evergreen.")
@click.option("--evg-api-config", type=str, required=True,
              help="Location of evergreen api configuration.")
@click.option("--evg-project-config", type=str, default="etc/evergreen.yml",
              help="Location of Evergreen project configuration.")
@click.option("--output-file", type=str, help="Name of output file to write.")
@click.option("--verbose", is_flag=True, default=False, help="Enable verbose logging.")
def main(expansion_file: str, evg_api_config: str, evg_project_config: str, output_file: str,
         verbose: bool) -> None:
    """
    Generate task configuration for a build variant.
    \f
    :param expansion_file: Location of evergreen expansions for task.
    :param evg_api_config: Location of file containing evergreen API authentication information.
    :param evg_project_config: Location of file containing evergreen project configuration.
    :param output_file: Location to write generated configuration to.
    :param verbose: Should verbose logging be used.
    """
    enable_logging(verbose)

    end_date = datetime.utcnow().replace(microsecond=0)
    start_date = end_date - timedelta(days=LOOKBACK_DURATION_DAYS)

    evg_expansions = EvgExpansions.from_yaml_file(expansion_file)

    # pylint: disable=no-value-for-parameter
    def dependencies(binder: inject.Binder) -> None:
        binder.bind(EvgExpansions, evg_expansions)
        binder.bind(SuiteSplitConfig, evg_expansions.build_suite_split_config(start_date, end_date))
        binder.bind(SplitStrategy, greedy_division)
        binder.bind(FallbackStrategy, round_robin_fallback)
        binder.bind(GenTaskOptions, evg_expansions.build_evg_config_gen_options())
        binder.bind(EvergreenApi, RetryingEvergreenApi.get_api(config_file=evg_api_config))
        binder.bind(EvergreenProjectConfig, parse_evergreen_file(evg_project_config))
        binder.bind(GenerationConfiguration, GenerationConfiguration.from_yaml_file())
        binder.bind(ResmokeProxyConfig,
                    ResmokeProxyConfig(resmoke_suite_dir=DEFAULT_TEST_SUITE_DIR))

    inject.configure(dependencies)

    orchestrator = GenerateBuildVariantOrchestrator()  # pylint: disable=no-value-for-parameter
    start_time = perf_counter()
    orchestrator.generate(evg_expansions.task_id, evg_expansions.build_variant, output_file)
    end_time = perf_counter()

    LOGGER.info("Total runtime", duration=end_time - start_time)


if __name__ == '__main__':
    main()  # pylint: disable=no-value-for-parameter
