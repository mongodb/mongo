"""Task generation for split resmoke tasks."""
import os
import re
from typing import Set, Any, Dict, NamedTuple, Optional, List, Match

import inject
import structlog
from shrub.v2 import Task, TaskDependency

from buildscripts.patch_builds.task_generation import resmoke_commands
from buildscripts.task_generation.suite_split import GeneratedSuite, SubSuite
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions
from buildscripts.task_generation.timeout import TimeoutEstimate
from buildscripts.util import taskname

LOGGER = structlog.getLogger(__name__)


def string_contains_any_of_args(string: str, args: List[str]) -> bool:
    """
    Return whether array contains any of a group of args.

    :param string: String being checked.
    :param args: Args being analyzed.
    :return: True if any args are found in the string.
    """
    return any(arg in string for arg in args)


class ResmokeGenTaskParams(NamedTuple):
    """
    Parameters describing how a specific resmoke suite should be generated.

    use_large_distro: Whether generated tasks should be run on a "large" distro.
    require_multiversion: Requires downloading Multiversion binaries.
    repeat_suites: How many times generated suites should be repeated.
    resmoke_args: Arguments to pass to resmoke in generated tasks.
    resmoke_jobs_max: Max number of jobs that resmoke should execute in parallel.
    depends_on: List of tasks this task depends on.
    """

    use_large_distro: bool
    large_distro_name: Optional[str]
    require_multiversion: Optional[bool]
    repeat_suites: int
    resmoke_args: str
    resmoke_jobs_max: Optional[int]
    config_location: str

    def generate_resmoke_args(self, suite_file: str, suite_name: str) -> str:
        """
        Generate the resmoke args for the given suite.

        :param suite_file: File containing configuration for test suite.
        :param suite_name: Name of suite being generated.
        :return: arguments to pass to resmoke.
        """
        resmoke_args = (f"--suite={suite_file}.yml --originSuite={suite_name} "
                        f" {self.resmoke_args}")
        if self.repeat_suites and not string_contains_any_of_args(resmoke_args,
                                                                  ["repeatSuites", "repeat"]):
            resmoke_args += f" --repeatSuites={self.repeat_suites} "

        return resmoke_args


class ResmokeGenTaskService:
    """A service to generated split resmoke suites."""

    @inject.autoparams()
    def __init__(self, gen_task_options: GenTaskOptions) -> None:
        """
        Initialize the service.

        :param gen_task_options: Global options for how tasks should be generated.
        """
        self.gen_task_options = gen_task_options

    def generate_tasks(self, generated_suite: GeneratedSuite,
                       params: ResmokeGenTaskParams) -> Set[Task]:
        """
        Build a set of shrub task for all the sub tasks.

        :param generated_suite: Suite to generate tasks for.
        :param params: Parameters describing how tasks should be generated.
        :return: Set of shrub tasks to generate the given suite.
        """
        tasks = {
            self._create_sub_task(suite, generated_suite, params)
            for suite in generated_suite.sub_suites
        }

        if self.gen_task_options.create_misc_suite:
            # Add the misc suite
            misc_task_name = f"{generated_suite.task_name}_misc_{generated_suite.build_variant}"
            tasks.add(
                self._generate_task(None, misc_task_name, TimeoutEstimate.no_timeouts(), params,
                                    generated_suite))

        return tasks

    def _create_sub_task(self, sub_suite: SubSuite, suite: GeneratedSuite,
                         params: ResmokeGenTaskParams) -> Task:
        """
        Create the sub task for the given suite.

        :param sub_suite: Sub-Suite to generate.
        :param suite: Parent suite being created.
        :param params: Parameters describing how tasks should be generated.
        :return: Shrub configuration for the sub-suite.
        """
        sub_task_name = taskname.name_generated_task(suite.task_name, sub_suite.index, len(suite),
                                                     suite.build_variant)
        return self._generate_task(sub_suite.index, sub_task_name, sub_suite.get_timeout_estimate(),
                                   params, suite)

    def _generate_task(self, sub_suite_index: Optional[int], sub_task_name: str,
                       timeout_est: TimeoutEstimate, params: ResmokeGenTaskParams,
                       suite: GeneratedSuite) -> Task:
        """
        Generate a shrub evergreen config for a resmoke task.

        :param sub_suite_index: Index of suite being generated.
        :param sub_task_name: Name of task to generate.
        :param timeout_est: Estimated runtime to use for calculating timeouts.
        :param params: Parameters describing how tasks should be generated.
        :param suite: Parent suite being created.
        :return: Shrub configuration for the described task.
        """
        # pylint: disable=too-many-arguments
        LOGGER.debug("Generating task", suite=suite.display_task_name(), index=sub_suite_index)

        target_suite_file = self.gen_task_options.suite_location(
            suite.sub_suite_config_file(sub_suite_index))
        run_tests_vars = self._get_run_tests_vars(target_suite_file, suite.suite_name, params)

        require_multiversion = params.require_multiversion
        timeout_cmd = timeout_est.generate_timeout_cmd(self.gen_task_options.is_patch,
                                                       params.repeat_suites,
                                                       self.gen_task_options.use_default_timeouts)
        commands = resmoke_commands("run generated tests", run_tests_vars, timeout_cmd,
                                    require_multiversion)

        return Task(sub_task_name, commands, self._get_dependencies())

    @staticmethod
    def _get_run_tests_vars(
            suite_file: str,
            suite_name: str,
            params: ResmokeGenTaskParams,
    ) -> Dict[str, Any]:
        """
        Generate a dictionary of the variables to pass to the task.

        :param suite_file: Suite being generated.
        :param suite_name: Name of suite being generated
        :param params: Parameters describing how tasks should be generated.
        :return: Dictionary containing variables and value to pass to generated task.
        """
        variables = {
            "resmoke_args": params.generate_resmoke_args(suite_file, suite_name),
            "gen_task_config_location": params.config_location,
        }

        if params.resmoke_jobs_max:
            variables["resmoke_jobs_max"] = params.resmoke_jobs_max

        if params.require_multiversion:
            variables["require_multiversion"] = params.require_multiversion

        return variables

    @staticmethod
    def _get_dependencies() -> Set[TaskDependency]:
        """Get the set of dependency tasks for these suites."""
        dependencies = {TaskDependency("archive_dist_test_debug")}
        return dependencies
