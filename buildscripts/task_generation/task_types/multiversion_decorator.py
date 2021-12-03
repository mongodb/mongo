"""Multiversion decorator for basic generated tasks."""
import copy
import os.path
from typing import List, Set, Union, Optional, NamedTuple

import inject
import structlog
from shrub.v2 import Task, FunctionCall
from shrub.v2.command import ShrubCommand

from buildscripts.task_generation.constants import DO_MULTIVERSION_SETUP, CONFIGURE_EVG_CREDENTIALS, RUN_GENERATED_TESTS
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyService
from buildscripts.task_generation.task_types.models.resmoke_task_model import ResmokeTask
from buildscripts.util import taskname
from buildscripts.util.teststats import normalize_test_name

LOGGER = structlog.get_logger(__name__)


class _SuiteFixtureType:
    """Suite fixture types."""

    SHELL = "shell"
    REPL = "repl"
    SHARD = "shard"
    OTHER = "other"


class MultiversionDecoratorParams(NamedTuple):
    """Parameters for converting tasks into multiversion tasks."""

    base_suite: str
    task: str
    variant: str
    num_tasks: int


class MultiversionGenTaskDecorator:
    """Multiversion decorator for basic generated tasks."""

    # pylint: disable=no-self-use
    @inject.autoparams()
    def __init__(self, resmoke_proxy: ResmokeProxyService):
        """Initialize multiversion decorator."""
        self.resmoke_proxy = resmoke_proxy
        self.old_versions = self._init_old_versions()

    def decorate_tasks_with_dynamically_generated_files(
            self, sub_tasks: Set[Task], params: MultiversionDecoratorParams) -> Set[Task]:
        """
        Make multiversion subtasks based on generated subtasks, for tasks with generated files. E.g. fuzzers.

        @param sub_tasks: set of existing sub-tasks to be converted to multiversion.
        @param params: decoration parameters.
        @return: Set of multiversion tasks.
        """
        fixture_type = self._get_suite_fixture_type(params.base_suite)
        versions_combinations = self._get_versions_combinations(fixture_type)

        decorated_tasks = set()
        for old_version in self.old_versions:
            for mixed_bin_versions in versions_combinations:
                for index, sub_task in enumerate(sub_tasks):
                    commands = list(sub_task.commands)
                    base_task_name = self._build_name(params.task, old_version, mixed_bin_versions)
                    sub_task_name = taskname.name_generated_task(base_task_name, index,
                                                                 params.num_tasks, params.variant)
                    suite_name = self._build_name(params.base_suite, old_version,
                                                  mixed_bin_versions)
                    self._update_execution_task_suite_info(commands, suite_name, old_version)
                    commands = self._add_multiversion_commands(commands)
                    decorated_tasks.add(
                        Task(name=sub_task_name, commands=commands,
                             dependencies=sub_task.dependencies))
        return decorated_tasks

    def decorate_tasks_with_explicit_files(
            self, sub_tasks: List[ResmokeTask],
            params: MultiversionDecoratorParams) -> List[ResmokeTask]:
        """
        Make multiversion subtasks based on generated subtasks for explicit tasks.

        Explicit tasks need to have new resmoke.py suite files created for each one with a
        unique list of test roots.
        """
        fixture_type = self._get_suite_fixture_type(params.base_suite)
        versions_combinations = self._get_versions_combinations(fixture_type)

        decorated_tasks = []
        for old_version in self.old_versions:
            for mixed_bin_versions in versions_combinations:
                for index, sub_task in enumerate(sub_tasks):
                    shrub_task = sub_task.shrub_task
                    commands = list(shrub_task.commands)

                    # Decorate the task name.
                    base_task_name = self._build_name(params.task, old_version, mixed_bin_versions)
                    sub_task_name = taskname.name_generated_task(base_task_name, index,
                                                                 params.num_tasks, params.variant)

                    # Decorate the suite name
                    resmoke_suite_name = self._build_name(sub_task.resmoke_suite_name, old_version,
                                                          mixed_bin_versions)
                    all_tests = [
                        normalize_test_name(test)
                        for test in self.resmoke_proxy.list_tests(resmoke_suite_name)
                    ]
                    test_list = [test for test in sub_task.test_list if test in all_tests]
                    excludes = [test for test in sub_task.excludes if test in all_tests]
                    execution_task_suite_name = taskname.name_generated_task(
                        resmoke_suite_name, index, params.num_tasks)
                    execution_task_suite_yaml_dir = os.path.dirname(
                        sub_task.execution_task_suite_yaml_path)
                    execution_task_suite_yaml_file = f"{execution_task_suite_name}.yml"
                    execution_task_suite_yaml_path = os.path.join(execution_task_suite_yaml_dir,
                                                                  execution_task_suite_yaml_file)

                    # Decorate the command invocation options.
                    self._update_execution_task_suite_info(commands, execution_task_suite_yaml_path,
                                                           old_version)
                    commands = self._add_multiversion_commands(commands)

                    # Store the result.
                    shrub_task = Task(name=sub_task_name, commands=commands,
                                      dependencies=shrub_task.dependencies)
                    decorated_tasks.append(
                        ResmokeTask(shrub_task=shrub_task, resmoke_suite_name=resmoke_suite_name,
                                    execution_task_suite_yaml_name=execution_task_suite_yaml_file,
                                    execution_task_suite_yaml_path=execution_task_suite_yaml_path,
                                    test_list=test_list, excludes=excludes))

        return decorated_tasks

    def decorate_single_multiversion_tasks(self, sub_tasks: List[ResmokeTask]):
        """Decorate a multiversion version of a task without all multiversion combinations."""
        decorated_sub_tasks = []
        for sub_task in sub_tasks:
            shrub_task = sub_task.shrub_task
            commands = self._add_multiversion_commands(shrub_task.commands)
            shrub_task = Task(name=shrub_task.name, commands=commands,
                              dependencies=shrub_task.dependencies)
            decorated_sub_tasks.append(
                ResmokeTask(shrub_task=shrub_task, resmoke_suite_name=sub_task.resmoke_suite_name,
                            execution_task_suite_yaml_path=sub_task.execution_task_suite_yaml_path,
                            execution_task_suite_yaml_name=sub_task.execution_task_suite_yaml_name,
                            test_list=sub_task.test_list, excludes=sub_task.excludes))
        return decorated_sub_tasks

    @staticmethod
    def _init_old_versions() -> List[str]:
        from buildscripts.resmokelib import multiversionconstants
        if multiversionconstants.LAST_LTS_FCV == multiversionconstants.LAST_CONTINUOUS_FCV:
            LOGGER.debug("Last-lts FCV and last-continuous FCV are equal")
            old_versions = ["last_lts"]
        else:
            old_versions = ["last_lts", "last_continuous"]
        LOGGER.debug("Determined old versions for the multiversion tasks",
                     old_versions=old_versions)
        return old_versions

    def _get_suite_fixture_type(self, suite_name: str) -> str:
        """Return suite fixture type."""
        source_config = self.resmoke_proxy.read_suite_config(suite_name)
        if "fixture" not in source_config["executor"]:
            return _SuiteFixtureType.SHELL
        if source_config["executor"]["fixture"]["class"] == "ShardedClusterFixture":
            return _SuiteFixtureType.SHARD
        if source_config["executor"]["fixture"]["class"] == "ReplicaSetFixture":
            return _SuiteFixtureType.REPL
        return _SuiteFixtureType.OTHER

    @staticmethod
    def _get_versions_combinations(fixture_type: str) -> List[str]:
        return {
            _SuiteFixtureType.SHELL: [""],
            _SuiteFixtureType.SHARD: ["new_old_old_new"],
            _SuiteFixtureType.REPL: ["new_new_old", "new_old_new", "old_new_new"],
            _SuiteFixtureType.OTHER: [""],
        }[fixture_type]

    @staticmethod
    def _build_name(base_name: str, old_version: str, mixed_bin_versions: str) -> str:
        return "_".join(part for part in [base_name, old_version, mixed_bin_versions] if part)

    @staticmethod
    def _find_command(name: str, commands: List[Union[FunctionCall, ShrubCommand]]
                      ) -> (Optional[int], Optional[FunctionCall]):
        for index, command in enumerate(commands):
            if hasattr(command, "name") and command.name == name:
                return index, command
        return None, None

    def _update_execution_task_suite_info(self, commands: List[Union[FunctionCall, ShrubCommand]],
                                          multiversion_suite_path: str, old_bin_version: str):
        index, run_test_func = self._find_command(RUN_GENERATED_TESTS, commands)
        if run_test_func is not None:
            run_test_vars = copy.deepcopy(run_test_func.parameters)
            run_test_vars["suite"] = multiversion_suite_path
            run_test_vars["multiversion_exclude_tags_version"] = old_bin_version
            commands[index] = FunctionCall(RUN_GENERATED_TESTS, run_test_vars)
            return run_test_vars["suite"]
        return None

    @staticmethod
    def _add_multiversion_commands(commands: List[Union[FunctionCall, ShrubCommand]]
                                   ) -> List[Union[FunctionCall, ShrubCommand]]:
        res = [
            FunctionCall("git get project no modules"),
            FunctionCall("add git tag"),
        ]
        for command in commands:
            res.append(command)

            if hasattr(command, "name") and command.name == CONFIGURE_EVG_CREDENTIALS:
                # Run multiversion setup after getting EVG credentials.
                res.append(FunctionCall(DO_MULTIVERSION_SETUP))
        return res
