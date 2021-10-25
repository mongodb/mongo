"""Task generation for split resmoke tasks."""
from typing import Set, Any, Dict, NamedTuple, Optional, List

import inject
import structlog
from shrub.v2 import Task, TaskDependency

from buildscripts.patch_builds.task_generation import resmoke_commands
from buildscripts.resmokelib.multiversionconstants import REQUIRES_FCV_TAG
from buildscripts.task_generation.constants import ARCHIVE_DIST_TEST_DEBUG_TASK, EXCLUDES_TAGS_FILE_PATH, \
    BACKPORT_REQUIRED_TAG
from buildscripts.task_generation.suite_split import GeneratedSuite
from buildscripts.task_generation.task_types.gentask_options import GenTaskOptions
from buildscripts.task_generation.timeout import TimeoutEstimate

LOGGER = structlog.getLogger(__name__)

EXCLUDE_TAGS = f"{REQUIRES_FCV_TAG},multiversion_incompatible,{BACKPORT_REQUIRED_TAG}"


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
    require_multiversion_setup: Requires downloading Multiversion binaries.
    repeat_suites: How many times generated suites should be repeated.
    resmoke_args: Arguments to pass to resmoke in generated tasks.
    resmoke_jobs_max: Max number of jobs that resmoke should execute in parallel.
    depends_on: List of tasks this task depends on.
    config_location: S3 path to the generated config tarball. None if no generated config files.
    """

    use_large_distro: bool
    large_distro_name: Optional[str]
    require_multiversion_setup: Optional[bool]
    repeat_suites: int
    resmoke_args: str
    resmoke_jobs_max: Optional[int]
    config_location: Optional[str]


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
            self._create_sub_task(index, suite.get_timeout_estimate(), generated_suite, params)
            for index, suite in enumerate(generated_suite.sub_suites)
        }

        if self.gen_task_options.create_misc_suite:
            tasks.add(
                self._create_sub_task(None, est_timeout=TimeoutEstimate.no_timeouts(),
                                      suite=generated_suite, params=params))
        if params.require_multiversion_setup:
            # do the same thing as the fuzzer taks.
            pass
        return tasks

    def _create_sub_task(self, index: Optional[int], est_timeout: TimeoutEstimate,
                         suite: GeneratedSuite, params: ResmokeGenTaskParams) -> Task:
        """
        Create the sub task for the given suite.

        :param index: index of sub_suite.
        :param est_timeout: timeout estimate.
        :param suite: Parent suite being created.
        :param params: Parameters describing how tasks should be generated.
        :return: Shrub configuration for the sub-suite.
        """
        return self._generate_task(
            suite.sub_suite_config_file(index), suite.sub_suite_task_name(index), est_timeout,
            params, suite)

    def _generate_task(self, sub_suite_file, sub_task_name: str, timeout_est: TimeoutEstimate,
                       params: ResmokeGenTaskParams, suite: GeneratedSuite) -> Task:
        """
        Generate a shrub evergreen config for a resmoke task.

        :param sub_suite_file: Name of the suite file to run in the generated task.
        :param sub_task_name: Name of task to generate.
        :param timeout_est: Estimated runtime to use for calculating timeouts.
        :param params: Parameters describing how tasks should be generated.
        :param suite: Parent suite being created.
        :return: Shrub configuration for the described task.
        """
        # pylint: disable=too-many-arguments
        LOGGER.debug("Generating task running suite", sub_task_name=sub_task_name,
                     sub_suite_file=sub_suite_file)

        # Some splits don't generate new physical config files.
        if params.config_location is not None:
            sub_suite_file_path = self.gen_task_options.suite_location(sub_suite_file)
        else:
            sub_suite_file_path = None

        run_tests_vars = self._get_run_tests_vars(sub_suite_file_path=sub_suite_file_path,
                                                  suite_file=suite.suite_name,
                                                  task_name=suite.task_name, params=params)

        require_multiversion_setup = params.require_multiversion_setup
        timeout_cmd = timeout_est.generate_timeout_cmd(self.gen_task_options.is_patch,
                                                       params.repeat_suites,
                                                       self.gen_task_options.use_default_timeouts)
        commands = resmoke_commands("run generated tests", run_tests_vars, timeout_cmd,
                                    require_multiversion_setup)

        return Task(sub_task_name, commands, self._get_dependencies())

    @staticmethod
    def generate_resmoke_args(original_suite: str, task_name: str,
                              params: ResmokeGenTaskParams) -> str:
        """
        Generate the resmoke args for the given suite.

        :param original_suite: Name of source suite of the generated suite files.
        :param task_name: Name of the task.
        :param params: task generation parameters.

        :return: arguments to pass to resmoke.
        """

        resmoke_args = f"--originSuite={original_suite} {params.resmoke_args}"

        if params.repeat_suites and not string_contains_any_of_args(resmoke_args,
                                                                    ["repeatSuites", "repeat"]):
            resmoke_args += f" --repeatSuites={params.repeat_suites} "

        if params.require_multiversion_setup:
            tag_file = EXCLUDES_TAGS_FILE_PATH
            resmoke_args += f" --tagFile={tag_file}"
            resmoke_args += f" --excludeWithAnyTags={EXCLUDE_TAGS},{task_name}_{BACKPORT_REQUIRED_TAG} "

        return resmoke_args

    def _get_run_tests_vars(
            self,
            sub_suite_file_path: Optional[str],
            suite_file: str,
            task_name: str,
            params: ResmokeGenTaskParams,
    ) -> Dict[str, Any]:
        """
        Generate a dictionary of the variables to pass to the task.

        :param sub_suite_file_path: path to the generated suite file.
        :param suite_file: name of the original suite file.
        :param task_name: Name of the task.
        :param params: Parameters describing how tasks should be generated.
        :return: Dictionary containing variables and value to pass to generated task.
        """
        resmoke_args = self.generate_resmoke_args(original_suite=suite_file, task_name=task_name,
                                                  params=params)
        variables = {
            "suite": sub_suite_file_path if sub_suite_file_path else suite_file,
            "resmoke_args": resmoke_args,
            "require_multiversion_setup": params.require_multiversion_setup
        }

        if params.config_location is not None:
            variables["gen_task_config_location"] = params.config_location

        if params.resmoke_jobs_max:
            variables["resmoke_jobs_max"] = params.resmoke_jobs_max

        return variables

    @staticmethod
    def _get_dependencies() -> Set[TaskDependency]:
        """Get the set of dependency tasks for these suites."""
        dependencies = {TaskDependency(ARCHIVE_DIST_TEST_DEBUG_TASK)}
        return dependencies
