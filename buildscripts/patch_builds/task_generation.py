"""Utilities to help generate evergreen tasks."""
from typing import Optional, List

from shrub.command import CommandDefinition
from shrub.config import Configuration
from shrub.operations import CmdTimeoutUpdate
from shrub.task import TaskDependency
from shrub.variant import TaskSpec, DisplayTaskDefinition


def _cmd_by_name(cmd_name):
    """
    Create a command definition of a function with the given name.

    :param cmd_name: Name of function.
    :return: Command Definition for function.
    """
    return CommandDefinition().function(cmd_name)


def resmoke_commands(run_tests_fn_name, run_tests_vars, timeout_info, use_multiversion=None):
    """
    Create a list of commands to run a resmoke task.

    :param run_tests_fn_name: Name of function to run resmoke tests.
    :param run_tests_vars: Dictionary of variables to pass to run_tests function.
    :param timeout_info: Timeout info for task.
    :param use_multiversion: If True include multiversion setup.
    :return: List of commands to run a resmoke task.
    """
    commands = [
        timeout_info.cmd,
        _cmd_by_name("do setup"),
        _cmd_by_name("do multiversion setup") if use_multiversion else None,
        _cmd_by_name(run_tests_fn_name).vars(run_tests_vars),
    ]

    return [cmd for cmd in commands if cmd]


class TimeoutInfo(object):
    """Timeout information for a generated task."""

    def __init__(self, use_defaults, exec_timeout=None, timeout=None):
        """
        Create timeout information.

        :param use_defaults: Don't overwrite any timeouts.
        :param exec_timeout: Exec timeout value to overwrite.
        :param timeout: Timeout value to overwrite.
        """
        self.use_defaults = use_defaults
        self.exec_timeout = exec_timeout
        self.timeout = timeout

    @classmethod
    def default_timeout(cls):
        """Create an instance of TimeoutInfo that uses default timeouts."""
        return cls(True)

    @classmethod
    def overridden(cls, exec_timeout=None, timeout=None):
        """
        Create an instance of TimeoutInfo that overwrites timeouts.

        :param exec_timeout: Exec timeout value to overwrite.
        :param timeout: Timeout value to overwrite.
        :return: TimeoutInfo that overwrites given timeouts.
        """
        if not exec_timeout and not timeout:
            raise ValueError("Must override either 'exec_timeout' or 'timeout'")
        return cls(False, exec_timeout=exec_timeout, timeout=timeout)

    @property
    def cmd(self):
        """Create a command that sets timeouts as specified."""
        if not self.use_defaults:
            timeout_cmd = CmdTimeoutUpdate()
            if self.timeout:
                timeout_cmd.timeout(self.timeout)

            if self.exec_timeout:
                timeout_cmd.exec_timeout(self.exec_timeout)
            return timeout_cmd.validate().resolve()

        return None

    def __repr__(self):
        """Create a string representation for debugging."""
        if self.use_defaults:
            return "<No Timeout Override>"
        return f"<exec_timeout={self.exec_timeout}, timeout={self.timeout}>"


class TaskList(object):
    """A list of evergreen tasks to be generated together."""

    def __init__(self, evg_config: Configuration):
        """
        Create a list of evergreen tasks to create.

        :param evg_config: Evergreen configuration to add tasks to.
        """
        self.evg_config = evg_config
        self.task_specs = []
        self.task_names = []

    def add_task(self, name: str, commands: [CommandDefinition],
                 depends_on: Optional[List[str]] = None, distro: Optional[str] = None):
        """
        Add a new task to the task list.

        :param name: Name of task to add.
        :param commands: List of commands comprising task.
        :param depends_on: Any dependencies for the task.
        :param distro: Distro task should be run on.
        """
        task = self.evg_config.task(name)
        task.commands(commands)

        if depends_on:
            for dep in depends_on:
                task.dependency(TaskDependency(dep))

        task_spec = TaskSpec(name)
        if distro:
            task_spec.distro(distro)
        self.task_specs.append(task_spec)
        self.task_names.append(name)

    def display_task(self, display_name: str, existing_tasks: Optional[List[str]] = None) \
            -> DisplayTaskDefinition:
        """
        Create a display task for the list of tasks.

        Note: This function should be called after all calls to `add_task` have been done.

        :param display_name: Name of display tasks.
        :param existing_tasks: Any existing tasks that should be part of the display task.
        :return: Display task object.
        """
        execution_tasks = self.task_names
        if existing_tasks:
            execution_tasks.extend(existing_tasks)

        display_task = DisplayTaskDefinition(display_name).execution_tasks(execution_tasks)
        return display_task

    def add_to_variant(self, variant_name: str, display_name: Optional[str] = None,
                       existing_tasks: Optional[List[str]] = None):
        """
        Add this task list to a build variant.

        :param variant_name: Variant to add to.
        :param display_name: Display name to add tasks under.
        :param existing_tasks: Any existing tasks that should be added to the display group.
        """
        variant = self.evg_config.variant(variant_name)
        variant.tasks(self.task_specs)
        if display_name:
            variant.display_task(self.display_task(display_name, existing_tasks))
