"""Multiversion decorator for basic generated tasks."""
import copy
from typing import List, Set, Union, Optional

import inject
import structlog
from shrub.v2 import Task, FunctionCall
from shrub.v2.command import ShrubCommand

from buildscripts.task_generation.constants import DO_MULTIVERSION_SETUP, CONFIGURE_EVG_CREDENTIALS, RUN_GENERATED_TESTS
from buildscripts.task_generation.resmoke_proxy import ResmokeProxyService
from buildscripts.util import taskname

LOGGER = structlog.get_logger(__name__)


class _SuiteFixtureType:
    """Suite fixture types."""

    SHELL = "shell"
    REPL = "repl"
    SHARD = "shard"
    OTHER = "other"


class MultiversionGenTaskDecorator:
    """Multiversion decorator for basic generated tasks."""

    # pylint: disable=no-self-use
    @inject.autoparams()
    def __init__(self, resmoke_proxy: ResmokeProxyService):
        """Initialize multiversion decorator."""
        self.resmoke_proxy = resmoke_proxy
        self.old_versions = self._init_old_versions()

    def decorate_tasks(self, sub_tasks: Set[Task], params) -> Set[Task]:
        """Make multiversion subtasks based on generated subtasks."""
        fixture_type = self._get_suite_fixture_type(params.suite)
        versions_combinations = self._get_versions_combinations(fixture_type)

        decorated_tasks = set()
        for old_version in self.old_versions:
            for mixed_bin_versions in versions_combinations:
                for index, sub_task in enumerate(sub_tasks):
                    commands = list(sub_task.commands)
                    base_task_name = self._build_name(params.task_name, old_version,
                                                      mixed_bin_versions.replace("-", "_"))
                    sub_task_name = taskname.name_generated_task(base_task_name, index,
                                                                 params.num_tasks, params.variant)
                    suite_name = self._build_name(params.suite, old_version,
                                                  mixed_bin_versions.replace("-", "_"))
                    self._update_suite_name(commands, suite_name)
                    commands = self._add_multiversion_commands(commands)
                    decorated_tasks.add(
                        Task(name=sub_task_name, commands=commands,
                             dependencies=sub_task.dependencies))
        return decorated_tasks

    def _init_old_versions(self) -> List[str]:
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

    def _get_versions_combinations(self, fixture_type: str) -> List[str]:
        return {
            _SuiteFixtureType.SHELL: [""],
            _SuiteFixtureType.SHARD: ["new-old-old-new"],
            _SuiteFixtureType.REPL: ["new-new-old", "new-old-new", "old-new-new"],
            _SuiteFixtureType.OTHER: [""],
        }[fixture_type]

    def _build_name(self, base_name: str, *suffixes: str) -> str:
        parts = [base_name]
        parts.extend(suffixes)
        return "_".join(part for part in parts if part != "")

    def _find_command(self, name: str, commands: List[Union[FunctionCall, ShrubCommand]]
                      ) -> (Optional[int], Optional[FunctionCall]):
        for index, command in enumerate(commands):
            if hasattr(command, "name") and command.name == name:
                return index, command
        return None, None

    def _update_suite_name(self, commands: List[Union[FunctionCall, ShrubCommand]],
                           suite_name: str):
        index, run_test_func = self._find_command(RUN_GENERATED_TESTS, commands)
        if run_test_func is not None:
            run_test_vars = copy.deepcopy(run_test_func.parameters)
            run_test_vars["suite"] = suite_name
            commands[index] = FunctionCall(RUN_GENERATED_TESTS, run_test_vars)

    def _add_multiversion_commands(self, commands: List[Union[FunctionCall, ShrubCommand]]
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
