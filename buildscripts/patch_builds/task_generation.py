"""Utilities to help generate evergreen tasks."""
from __future__ import annotations

from typing import Any, Dict, Optional, List

from shrub.v2 import FunctionCall, ShrubProject
from shrub.v2.command import timeout_update, ShrubCommand
from structlog import get_logger

LOGGER = get_logger(__name__)
MAX_SHRUB_TASKS_FOR_SINGLE_TASK = 1000


def resmoke_commands(run_tests_fn_name: str, run_tests_vars: Dict[str, Any],
                     timeout_info: TimeoutInfo,
                     use_multiversion: Optional[str] = None) -> List[ShrubCommand]:
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
        FunctionCall("do setup"),
        FunctionCall("do multiversion setup") if use_multiversion else None,
        FunctionCall(run_tests_fn_name, run_tests_vars),
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
            return timeout_update(exec_timeout_secs=self.exec_timeout, timeout_secs=self.timeout)

        return None

    def __repr__(self):
        """Create a string representation for debugging."""
        if self.use_defaults:
            return "<No Timeout Override>"
        return f"<exec_timeout={self.exec_timeout}, timeout={self.timeout}>"


def validate_task_generation_limit(shrub_project: ShrubProject) -> bool:
    """
    Determine if this shrub configuration generates less than the limit.

    :param shrub_project: Shrub configuration to validate.
    :return: True if the configuration is under the limit.
    """
    tasks_to_create = len(shrub_project.all_tasks())
    if tasks_to_create > MAX_SHRUB_TASKS_FOR_SINGLE_TASK:
        LOGGER.warning("Attempting to create more tasks than max, aborting", tasks=tasks_to_create,
                       max=MAX_SHRUB_TASKS_FOR_SINGLE_TASK)
        return False
    return True
