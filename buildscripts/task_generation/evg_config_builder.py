"""Builder for generating evergreen configuration."""
from threading import Lock
from typing import Set, List, Dict

import inject
from shrub.v2 import ShrubProject, BuildVariant, ExistingTask, Task

from buildscripts.patch_builds.task_generation import validate_task_generation_limit
from buildscripts.task_generation.constants import ACTIVATE_ARCHIVE_DIST_TEST_DEBUG_TASK
from buildscripts.task_generation.gen_task_service import GenTaskService, \
    GenTaskOptions, ResmokeGenTaskParams, FuzzerGenTaskParams
from buildscripts.task_generation.generated_config import GeneratedFile, GeneratedConfiguration
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyService
from buildscripts.task_generation.suite_split import SuiteSplitService, GeneratedSuite, \
    SuiteSplitParameters
from buildscripts.task_generation.task_types.fuzzer_tasks import FuzzerTask

# pylint: disable=too-many-instance-attributes


class EvgConfigBuilder:
    """A builder class for building evergreen configuration."""

    @inject.autoparams()
    def __init__(
            self,
            resmoke_proxy: ResmokeProxyService,
            suite_split_service: SuiteSplitService,
            evg_config_gen_service: GenTaskService,
            gen_options: GenTaskOptions,
    ) -> None:
        """
        Initialize a new builder.

        :param resmoke_proxy: Proxy to access resmoke data.
        :param suite_split_service: Service to split suites into sub-suites.
        :param evg_config_gen_service: Service to generate evergreen configuration.
        :param gen_options: Global options for generating evergreen configuration.
        """
        self.resmoke_proxy = resmoke_proxy
        self.suite_split_service = suite_split_service
        self.evg_config_gen_service = evg_config_gen_service
        self.gen_options = gen_options

        self.shrub_config = ShrubProject.empty()
        self.build_variants: Dict[str, BuildVariant] = {}
        self.generated_files: List[GeneratedFile] = []
        self.lock = Lock()

    def get_build_variant(self, build_variant: str) -> BuildVariant:
        """
        Get the build variant object, creating it if it doesn't exist.

        NOTE: The `lock` should be held by any functions calling this one.

        :param build_variant: Name of build variant.
        :return: BuildVariant object being created.
        """
        if build_variant not in self.build_variants:
            self.build_variants[build_variant] = BuildVariant(build_variant, activate=False)
        return self.build_variants[build_variant]

    def generate_suite(self, split_params: SuiteSplitParameters,
                       gen_params: ResmokeGenTaskParams) -> None:
        """
        Add configuration to generate a split version of the specified resmoke suite.

        :param split_params: Parameters of how resmoke suite should be split.
        :param gen_params: Parameters of how evergreen configuration should be generated.
        """
        generated_suite = self.suite_split_service.split_suite(split_params)
        with self.lock:
            build_variant = self.get_build_variant(generated_suite.build_variant)
            resmoke_tasks = self.evg_config_gen_service.generate_task(generated_suite,
                                                                      build_variant, gen_params)
        self.generated_files.extend(self.resmoke_proxy.render_suite_files(resmoke_tasks))

    def generate_fuzzer(self, fuzzer_params: FuzzerGenTaskParams) -> FuzzerTask:
        """
        Add configuration to generate the specified fuzzer task.

        :param fuzzer_params: Parameters of how the fuzzer suite should generated.
        """
        with self.lock:
            build_variant = self.get_build_variant(fuzzer_params.variant)
            return self.evg_config_gen_service.generate_fuzzer_task(fuzzer_params, build_variant)

    def add_display_task(self, display_task_name: str, execution_task_names: Set[str],
                         build_variant: str) -> None:
        """
        Add configuration to generate the specified display task.

        :param display_task_name: Name of display task to create.
        :param execution_task_names: Name of execution tasks to include in display task.
        :param build_variant: Name of build variant to add to.
        """
        execution_tasks = {ExistingTask(task_name) for task_name in execution_task_names}
        with self.lock:
            build_variant = self.get_build_variant(build_variant)
            build_variant.display_task(display_task_name, execution_existing_tasks=execution_tasks)

    def generate_archive_dist_test_debug_activator_task(self, variant: str):
        """
        Generate dummy task to activate the task that archives debug symbols.

        We can't activate it directly as it's not generated.
        """
        with self.lock:
            build_variant = self.get_build_variant(variant)
            build_variant.add_existing_task(ExistingTask(ACTIVATE_ARCHIVE_DIST_TEST_DEBUG_TASK))

    def build(self, config_file_name: str) -> GeneratedConfiguration:
        """
        Build the specified configuration and return the files needed to create it.

        :param config_file_name: Filename to use for evergreen configuration.
        :return: Dictionary of files and contents that are needed to create configuration.
        """
        for build_variant in self.build_variants.values():
            self.shrub_config.add_build_variant(build_variant)
        if not validate_task_generation_limit(self.shrub_config):
            raise ValueError("Attempting to generate more than max tasks in single generator")

        self.generated_files.append(GeneratedFile(config_file_name, self.shrub_config.json()))
        return GeneratedConfiguration(self.generated_files)
