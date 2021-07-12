"""Tools for generating evergreen configuration."""
import os
import sys
from typing import Optional, List, Set

import inject
import structlog
from shrub.v2 import BuildVariant, Task
from evergreen import EvergreenApi

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.task_generation.task_types.multiversion_tasks import MultiversionGenTaskParams, \
    MultiversionGenTaskService
from buildscripts.task_generation.task_types.fuzzer_tasks import FuzzerGenTaskParams, FuzzerTask, \
    FuzzerGenTaskService
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions
from buildscripts.task_generation.task_types.resmoke_tasks import ResmokeGenTaskParams, \
    ResmokeGenTaskService
from buildscripts.task_generation.gen_config import GenerationConfiguration
from buildscripts.task_generation.suite_split import GeneratedSuite
# pylint: enable=wrong-import-position

LOGGER = structlog.getLogger(__name__)

NO_LARGE_DISTRO_ERR = """
***************************************************************************************
It appears we are trying to generate a task marked as requiring a large distro, but the
build variant has not specified a large build variant. In order to resolve this error,
you need to:

(1) add a "large_distro_name" expansion to this build variant ("{build_variant}").

 -- or --
 
(2) add this build variant ("{build_variant}") to the "build_variant_large_distro_exception"
list in the "etc/generate_subtasks_config.yml" file.
***************************************************************************************
"""


class GenTaskService:
    """A service for building evergreen task configurations."""

    # pylint: disable=too-many-arguments
    @inject.autoparams()
    def __init__(self, evg_api: EvergreenApi, gen_task_options: GenTaskOptions,
                 gen_config: GenerationConfiguration,
                 resmoke_gen_task_service: ResmokeGenTaskService,
                 multiversion_gen_task_service: MultiversionGenTaskService,
                 fuzzer_gen_task_service: FuzzerGenTaskService) -> None:
        """
        Initialize the service.

        :param evg_api: Evergreen API client.
        :param gen_task_options: Options for how tasks should be generated.
        :param gen_config:
        :param resmoke_gen_task_service: Service for generating standard resmoke tasks.
        :param multiversion_gen_task_service: Service for generating multiversion resmoke tasks.
        :param fuzzer_gen_task_service: Service for generating fuzzer tasks.
        """
        self.evg_api = evg_api
        self.gen_task_options = gen_task_options
        self.gen_config = gen_config
        self.resmoke_gen_task_service = resmoke_gen_task_service
        self.multiversion_gen_task_service = multiversion_gen_task_service
        self.fuzzer_gen_task_service = fuzzer_gen_task_service

    def generate_fuzzer_task(self, params: FuzzerGenTaskParams,
                             build_variant: BuildVariant) -> FuzzerTask:
        """
        Generate evergreen configuration for the given fuzzer and add it to the build_variant.

        :param params: Parameters for how fuzzer should be generated.
        :param build_variant: Build variant to add generated configuration to.
        """
        fuzzer_task = self.fuzzer_gen_task_service.generate_tasks(params)
        distros = self._get_distro(build_variant.name, params.use_large_distro,
                                   params.large_distro_name)
        if params.add_to_display_task:
            build_variant.display_task(fuzzer_task.task_name, fuzzer_task.sub_tasks,
                                       distros=distros, activate=False)
        else:
            build_variant.add_tasks(fuzzer_task.sub_tasks, distros=distros, activate=False)
        return fuzzer_task

    def generate_task(self, generated_suite: GeneratedSuite, build_variant: BuildVariant,
                      gen_params: ResmokeGenTaskParams) -> None:
        """
        Generate evergreen configuration for the given suite and add it to the build_variant.

        :param generated_suite: Suite to add.
        :param build_variant: Build variant to add generated configuration to.
        :param gen_params: Parameters to configuration how tasks are generated.
        """
        execution_tasks = self.resmoke_gen_task_service.generate_tasks(generated_suite, gen_params)
        distros = self._get_distro(build_variant.name, gen_params.use_large_distro,
                                   gen_params.large_distro_name)
        build_variant.display_task(generated_suite.display_task_name(),
                                   execution_tasks=execution_tasks, distros=distros, activate=False)

    def generate_multiversion_task(self, generated_suite: GeneratedSuite,
                                   build_variant: BuildVariant,
                                   gen_params: MultiversionGenTaskParams) -> None:
        """
        Generate evergreen configuration for the given suite and add it to the build_variant.

        :param generated_suite: Suite to add.
        :param build_variant: Build variant to add generated configuration to.
        :param gen_params: Parameters to configuration how tasks are generated.
        """
        execution_tasks = self.multiversion_gen_task_service.generate_tasks(
            generated_suite, gen_params)
        distros = self._get_distro(build_variant.name, gen_params.use_large_distro,
                                   gen_params.large_distro_name)
        build_variant.display_task(generated_suite.display_task_name(),
                                   execution_tasks=execution_tasks, distros=distros, activate=False)

    def generate_multiversion_burnin_task(self, generated_suite: GeneratedSuite,
                                          gen_params: MultiversionGenTaskParams,
                                          build_variant: BuildVariant) -> Set[Task]:
        """
        Generate burn_in configuration for the given suite and add it to the build_variant.

        :param generated_suite: Suite to add.
        :param build_variant: Build variant to add generated configuration to.
        :param gen_params: Parameters to configuration how tasks are generated.
        :return: Set of tasks that were generated.
        """
        tasks = self.multiversion_gen_task_service.generate_tasks(generated_suite, gen_params)
        distros = self._get_distro(build_variant.name, gen_params.use_large_distro,
                                   gen_params.large_distro_name)
        if gen_params.add_to_display_task:
            build_variant.display_task(generated_suite.task_name, tasks, distros=distros)
        else:
            build_variant.add_tasks(tasks, distros=distros)
        return tasks

    def _get_distro(self, build_variant: str, use_large_distro: bool,
                    large_distro_name: Optional[str]) -> Optional[List[str]]:
        """
        Get the distros that the tasks should be run on.

        :param build_variant: Name of build variant being generated.
        :param use_large_distro: Whether a large distro should be used.
        :return: List of distros to run on.
        """
        if use_large_distro:
            if large_distro_name:
                return [large_distro_name]

            if build_variant not in self.gen_config.build_variant_large_distro_exceptions:
                print(NO_LARGE_DISTRO_ERR.format(build_variant=build_variant))
                raise ValueError("Invalid Evergreen Configuration")

        return None
