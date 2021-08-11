"""Task generation for fuzzer tasks."""
from typing import NamedTuple, Set, Optional, Dict, List

from shrub.v2 import Task, FunctionCall, TaskDependency

from buildscripts.patch_builds.task_generation import TimeoutInfo
from buildscripts.util import taskname


def get_multiversion_resmoke_args(is_sharded: bool) -> str:
    """Return resmoke args used to configure a cluster for multiversion testing."""
    if is_sharded:
        return "--numShards=2 --numReplSetNodes=2 "
    return "--numReplSetNodes=3 --linearChain=on "


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
    require_multiversion: Requires downloading Multiversion binaries.
    use_large_distro: Should tests be generated on a large distro.
    add_to_display_task: Should generated tasks be grouped in a display task.
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
    require_multiversion: Optional[bool]
    use_large_distro: Optional[bool]
    large_distro_name: Optional[str]
    config_location: str
    add_to_display_task: bool = True
    version_config: Optional[List[str]] = None
    is_sharded: Optional[bool] = None

    def jstestfuzz_params(self) -> Dict[str, str]:
        """Build a dictionary of parameters to pass to jstestfuzz."""
        return {
            "jstestfuzz_vars": f"--numGeneratedFiles {self.num_files} {self.jstestfuzz_vars or ''}",
            "npm_command": self.npm_command,
        }

    def get_task_name(self, version: str) -> str:
        """Get the name to use for generated tasks."""
        if version:
            return f"{self.suite}_multiversion_{version}"
        return self.task_name

    def get_resmoke_args(self) -> str:
        """Get the resmoke arguments to use for generated tasks."""
        if self.is_sharded is not None:
            mv_args = get_multiversion_resmoke_args(self.is_sharded)
            return f"{self.resmoke_args or ''} --mixedBinVersions={self.version_config} {mv_args}"
        return self.resmoke_args


class FuzzerGenTaskService:
    """A service for generating fuzzer tasks."""

    def generate_tasks(self, params: FuzzerGenTaskParams) -> FuzzerTask:
        """
        Generate evergreen tasks for fuzzers based on the options given.

        :param params: Parameters for how task should be generated.
        :return: Set of shrub tasks.
        """
        version_list = params.version_config
        if version_list is None:
            version_list = [""]

        sub_tasks = set()
        for version in version_list:
            sub_tasks = sub_tasks.union({
                self.build_fuzzer_sub_task(index, params, version)
                for index in range(params.num_tasks)
            })

        return FuzzerTask(task_name=params.get_task_name(""), sub_tasks=sub_tasks)

    @staticmethod
    def build_fuzzer_sub_task(task_index: int, params: FuzzerGenTaskParams, version: str) -> Task:
        """
        Build a shrub task to run the fuzzer.

        :param task_index: Index of sub task being generated.
        :param params: Parameters describing how tasks should be generated.
        :param version: Multiversion version to generate against.
        :return: Shrub task to run the fuzzer.
        """
        sub_task_name = taskname.name_generated_task(
            params.get_task_name(version), task_index, params.num_tasks, params.variant)

        suite_arg = f"--suites={params.suite}"
        run_tests_vars = {
            "continue_on_failure": params.continue_on_failure,
            "resmoke_args": f"{suite_arg} {params.get_resmoke_args()}",
            "resmoke_jobs_max": params.resmoke_jobs_max,
            "should_shuffle": params.should_shuffle,
            "require_multiversion": params.require_multiversion,
            "timeout_secs": params.timeout_secs,
            "task": params.get_task_name(version),
            "gen_task_config_location": params.config_location,
        }  # yapf: disable

        timeout_info = TimeoutInfo.overridden(timeout=params.timeout_secs)

        commands = [
            timeout_info.cmd,
            FunctionCall("do setup"),
            FunctionCall("configure evergreen api credentials")
            if params.require_multiversion else None,
            FunctionCall("do multiversion setup") if params.require_multiversion else None,
            FunctionCall("setup jstestfuzz"),
            FunctionCall("run jstestfuzz", params.jstestfuzz_params()),
            FunctionCall("run generated tests", run_tests_vars)
        ]
        commands = [command for command in commands if command is not None]

        return Task(sub_task_name, commands, {TaskDependency("archive_dist_test_debug")})
