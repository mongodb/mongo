"""Task generation for fuzzer tasks."""
from typing import NamedTuple, Set, Optional, Dict

from shrub.v2 import Task, FunctionCall, TaskDependency

from buildscripts.patch_builds.task_generation import TimeoutInfo
from buildscripts.task_generation.constants import CONFIGURE_EVG_CREDENTIALS, \
    RUN_GENERATED_TESTS
from buildscripts.task_generation.task_types.multiversion_decorator import MultiversionGenTaskDecorator, \
    MultiversionDecoratorParams
from buildscripts.util import taskname


class FuzzerTask(NamedTuple):
    """
    Evergreen configuration for a generated fuzzer command.

    task_name: Name of fuzzer task that was generated.
    sub_tasks: Set of sub-tasks composing the fuzzer task.
    """

    task_name: str
    sub_tasks: Set[Task]


class FuzzerGenTaskParams(NamedTuple):
    """
    Parameters to generate a fuzzer task.

    task_name: Name of task being generated.
    variant: Name of build variant being generated on.
    suite: Resmoke suite for generated tests.
    num_files: Number of javascript files fuzzer should generate.
    num_tasks: Number of sub-tasks fuzzer should generate.
    resmoke_args: Arguments to pass to resmoke invocation.
    npm_command: NPM command to perform fuzzer execution.
    jstestfuzz_vars: Arguments to pass to fuzzer invocation.
    continue_on_failure: Should generated tests continue running after hitting an error.
    resmoke_jobs_max: Maximum number of jobs resmoke should execute in parallel.
    should_shuffle: Should tests be executed out of order.
    timeout_secs: Timeout before test execution is considered hung.
    require_multiversion_setup: Requires downloading Multiversion binaries.
    use_large_distro: Should tests be generated on a large distro.
    config_location: S3 path to the generated config tarball. None if no generated config files.
    dependencies: Set of dependencies generated tasks should depend on.
    """

    task_name: str
    variant: str
    suite: str
    num_files: int
    num_tasks: int
    resmoke_args: str
    npm_command: str
    jstestfuzz_vars: Optional[str]
    continue_on_failure: bool
    resmoke_jobs_max: int
    should_shuffle: bool
    timeout_secs: int
    require_multiversion_setup: Optional[bool]
    use_large_distro: Optional[bool]
    large_distro_name: Optional[str]
    config_location: str
    dependencies: Set[str]

    def jstestfuzz_params(self) -> Dict[str, str]:
        """Build a dictionary of parameters to pass to jstestfuzz."""
        return {
            "jstestfuzz_vars": f"--numGeneratedFiles {self.num_files} {self.jstestfuzz_vars or ''}",
            "npm_command": self.npm_command,
        }

    def get_resmoke_args(self) -> str:
        """Get the resmoke arguments to use for generated tasks."""
        return self.resmoke_args


class FuzzerGenTaskService:
    """A service for generating fuzzer tasks."""

    def __init__(self):
        """Initialize the service."""
        self.multiversion_decorator = MultiversionGenTaskDecorator()  # pylint: disable=no-value-for-parameter

    def generate_tasks(self, params: FuzzerGenTaskParams) -> FuzzerTask:
        """
        Generate evergreen tasks for fuzzers based on the options given.

        :param params: Parameters for how task should be generated.
        :return: Set of shrub tasks.
        """
        sub_tasks = set()
        sub_tasks = sub_tasks.union(
            {self.build_fuzzer_sub_task(index, params)
             for index in range(params.num_tasks)})

        if params.require_multiversion_setup:
            mv_params = MultiversionDecoratorParams(
                base_suite=params.suite,
                task=params.task_name,
                variant=params.variant,
                num_tasks=params.num_tasks,
            )
            sub_tasks = self.multiversion_decorator.decorate_tasks_with_dynamically_generated_files(
                sub_tasks, mv_params)

        return FuzzerTask(task_name=params.task_name, sub_tasks=sub_tasks)

    @staticmethod
    def build_fuzzer_sub_task(task_index: int, params: FuzzerGenTaskParams) -> Task:
        """
        Build a shrub task to run the fuzzer.

        :param task_index: Index of sub task being generated.
        :param params: Parameters describing how tasks should be generated.
        :return: Shrub task to run the fuzzer.
        """
        sub_task_name = taskname.name_generated_task(params.task_name, task_index, params.num_tasks,
                                                     params.variant)

        run_tests_vars = {
            "continue_on_failure": params.continue_on_failure,
            "resmoke_args": params.get_resmoke_args(),
            "resmoke_jobs_max": params.resmoke_jobs_max,
            "should_shuffle": params.should_shuffle,
            "require_multiversion_setup": params.require_multiversion_setup,
            "timeout_secs": params.timeout_secs,
            "task": params.task_name,
            # This expansion's name was shortened to reduce the overall size of
            # the generated configuration file
            # gtcl = gen_task_config_location
            "gtcl": params.config_location,
            "suite": params.suite,
        }  # yapf: disable

        timeout_info = TimeoutInfo.overridden(timeout=params.timeout_secs)

        commands = [
            timeout_info.cmd,
            FunctionCall("do setup"),
            FunctionCall(CONFIGURE_EVG_CREDENTIALS),
            FunctionCall("setup jstestfuzz"),
            FunctionCall("run jstestfuzz", params.jstestfuzz_params()),
            FunctionCall(RUN_GENERATED_TESTS, run_tests_vars),
            FunctionCall("minimize jstestfuzz")
        ]

        dependencies = {TaskDependency(dependency) for dependency in params.dependencies}

        return Task(sub_task_name, commands, dependencies)
