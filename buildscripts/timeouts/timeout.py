"""Timeout information for generating tasks."""
from __future__ import annotations

import math
from datetime import timedelta
from inspect import currentframe, getframeinfo
from typing import NamedTuple, Optional

import structlog
from shrub.v2.command import timeout_update

LOGGER = structlog.getLogger(__name__)

AVG_TASK_SETUP_TIME = int(timedelta(minutes=2).total_seconds())
MIN_TIMEOUT_SECONDS = int(timedelta(minutes=5).total_seconds())
MAX_EXPECTED_TIMEOUT = int(timedelta(hours=48).total_seconds())
DEFAULT_SCALING_FACTOR = 3.0


def calculate_timeout(avg_runtime: float, scaling_factor: Optional[float] = None) -> int:
    """
    Determine how long a runtime to set based on average runtime and a scaling factor.

    :param avg_runtime: Average runtime of previous runs.
    :param scaling_factor: Scaling factor for timeout.
    :return: timeout to use (in seconds).
    """

    scaling_factor = DEFAULT_SCALING_FACTOR if not scaling_factor else scaling_factor

    def round_to_minute(runtime):
        """Round the given seconds up to the nearest minute."""
        distance_to_min = 60 - (runtime % 60)
        return int(math.ceil(runtime + distance_to_min))  # pylint: disable=c-extension-no-member

    return max(MIN_TIMEOUT_SECONDS, round_to_minute(avg_runtime * scaling_factor))


class TimeoutEstimate(NamedTuple):
    """Runtime estimates used to calculate timeouts."""

    max_test_runtime: Optional[float]
    expected_task_runtime: Optional[float]

    @classmethod
    def no_timeouts(cls) -> "TimeoutEstimate":
        """Create an instance with no estimation data."""
        return cls(max_test_runtime=None, expected_task_runtime=None)

    def is_specified(self) -> bool:
        """Determine if any specific timeout value has been specified."""
        return self.max_test_runtime is not None or self.expected_task_runtime is not None

    def calculate_test_timeout(self, repeat_factor: int,
                               scaling_factor: Optional[float] = None) -> Optional[int]:
        """
        Calculate the timeout to use for tests.

        :param repeat_factor: How many times the suite will be repeated.
        :param scaling_factor: Scaling factor for timeout.
        :return: Timeout value to use for tests.
        """
        if self.max_test_runtime is None:
            return None

        timeout = calculate_timeout(self.max_test_runtime, scaling_factor) * repeat_factor
        LOGGER.debug("Setting timeout", timeout=timeout, max_runtime=self.max_test_runtime,
                     repeat_factor=repeat_factor, scaling_factor=(scaling_factor
                                                                  or DEFAULT_SCALING_FACTOR))
        return timeout

    def calculate_task_timeout(self, repeat_factor: int,
                               scaling_factor: Optional[float] = None) -> Optional[int]:
        """
        Calculate the timeout to use for tasks.

        :param repeat_factor: How many times the suite will be repeated.
        :param scaling_factor: Scaling factor for timeout.
        :return: Timeout value to use for tasks.
        """
        if self.expected_task_runtime is None:
            return None

        exec_timeout = calculate_timeout(self.expected_task_runtime,
                                         scaling_factor) * repeat_factor + AVG_TASK_SETUP_TIME
        LOGGER.debug("Setting exec_timeout", exec_timeout=exec_timeout,
                     suite_runtime=self.expected_task_runtime, repeat_factor=repeat_factor,
                     scaling_factor=(scaling_factor or DEFAULT_SCALING_FACTOR))
        return exec_timeout

    def generate_timeout_cmd(
            self, is_patch: bool, repeat_factor: int, test_timeout_factor: Optional[float] = None,
            task_timeout_factor: Optional[float] = None, use_default: bool = False) -> TimeoutInfo:
        """
        Create the timeout info to use to create a timeout shrub command.

        :param is_patch: Whether the command is being created in a patch build.
        :param repeat_factor: How many times the suite will be repeated.
        :param test_timeout_factor: Scaling factor for test timeout.
        :param task_timeout_factor: Scaling factor for task timeout.
        :param use_default: Should the default timeout be used.
        :return: Timeout info for the task.
        """

        if not self.is_specified or use_default:
            return TimeoutInfo.default_timeout()

        test_timeout = self.calculate_test_timeout(repeat_factor, test_timeout_factor)
        task_timeout = self.calculate_task_timeout(repeat_factor, task_timeout_factor)

        if is_patch and (test_timeout > MAX_EXPECTED_TIMEOUT
                         or task_timeout > MAX_EXPECTED_TIMEOUT):
            frameinfo = getframeinfo(currentframe())
            LOGGER.error(
                "This task looks like it is expected to run far longer than normal. This is "
                "likely due to setting the suite 'repeat' value very high. If you are sure "
                "this is something you want to do, comment this check out in your patch build "
                "and resubmit", repeat_value=repeat_factor, timeout=test_timeout,
                exec_timeout=task_timeout, code_file=frameinfo.filename, code_line=frameinfo.lineno,
                max_timeout=MAX_EXPECTED_TIMEOUT)
            raise ValueError("Failing due to expected runtime.")

        return TimeoutInfo.overridden(timeout=test_timeout, exec_timeout=task_timeout)


class TimeoutInfo(object):
    """Timeout information for a task."""

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
