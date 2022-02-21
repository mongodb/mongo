"""Utilities to help generate evergreen tasks."""
from __future__ import annotations

from typing import Any, Dict, Optional, List

from shrub.v2 import FunctionCall, ShrubProject
from shrub.v2.command import timeout_update, ShrubCommand
from structlog import get_logger

from buildscripts.task_generation.constants import CONFIGURE_EVG_CREDENTIALS, DO_MULTIVERSION_SETUP

LOGGER = get_logger(__name__)
MAX_SHRUB_TASKS_FOR_SINGLE_TASK = 1000

MAX_GEN_TASKS_ERR = """
********************************************************************************
It appears we are trying to generate more tasks than are supported by burn_in.
This likely means that a large number of tests have been changed in this patch
build. 

burn_in supports of a max of {max_tasks} and your patch requires {patch_tasks}.

If you would like to validate your changes with burn_in, you will need to break
your patch up into patches that each touch a fewer number of tests.
********************************************************************************
"""


def resmoke_commands(run_tests_fn_name: str, run_tests_vars: Dict[str, Any],
                     timeout_info: TimeoutInfo,
                     require_multiversion_setup: Optional[bool] = False) -> List[ShrubCommand]:
    """
    Create a list of commands to run a resmoke task.

    Used by burn_in* only. Other tasks use a standalone multiversion decorator.

    :param run_tests_fn_name: Name of function to run resmoke tests.
    :param run_tests_vars: Dictionary of variables to pass to run_tests function.
    :param timeout_info: Timeout info for task.
    :param require_multiversion_setup: Requires downloading Multiversion binaries.
    :return: List of commands to run a resmoke task.
    """

    commands = [
        timeout_info.cmd,
        FunctionCall("git get project no modules") if require_multiversion_setup else None,
        FunctionCall("add git tag") if require_multiversion_setup else None,
        FunctionCall("do setup"),
        FunctionCall(CONFIGURE_EVG_CREDENTIALS),
        FunctionCall(DO_MULTIVERSION_SETUP) if require_multiversion_setup else None,
        FunctionCall(run_tests_fn_name, run_tests_vars),
        FunctionCall("validate resmoke tests runtime"),
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
        print(
            MAX_GEN_TASKS_ERR.format(max_tasks=MAX_SHRUB_TASKS_FOR_SINGLE_TASK,
                                     patch_tasks=tasks_to_create))
        LOGGER.warning("Attempting to create more tasks than max, aborting", tasks=tasks_to_create,
                       max=MAX_SHRUB_TASKS_FOR_SINGLE_TASK)
        return False
    return True
