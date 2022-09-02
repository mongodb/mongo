#!/usr/bin/env python3
"""Determine the timeout value a task should use in evergreen."""
from __future__ import annotations

import argparse
import math
import os
import shlex
import sys
from datetime import datetime, timedelta
from pathlib import Path
from typing import Dict, List, Optional

import inject
import structlog
import yaml
from pydantic import BaseModel
from evergreen import EvergreenApi, RetryingEvergreenApi

from buildscripts.ciconfig.evergreen import (EvergreenProjectConfig, parse_evergreen_file)
from buildscripts.resmoke_proxy.resmoke_proxy import ResmokeProxyService
from buildscripts.timeouts.timeout_service import (TimeoutParams, TimeoutService, TimeoutSettings)
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.taskname import determine_task_base_name

LOGGER = structlog.get_logger(__name__)
DEFAULT_TIMEOUT_OVERRIDES = "etc/evergreen_timeouts.yml"
DEFAULT_EVERGREEN_CONFIG = "etc/evergreen.yml"
DEFAULT_EVERGREEN_AUTH_CONFIG = "~/.evergreen.yml"
COMMIT_QUEUE_ALIAS = "__commit_queue"
UNITTEST_TASK = "run_unittests"
IGNORED_SUITES = {
    "integration_tests_replset", "integration_tests_replset_ssl_auth", "integration_tests_sharded",
    "integration_tests_standalone", "integration_tests_standalone_audit", "mongos_test",
    "server_selection_json_test"
}
HISTORY_LOOKBACK = timedelta(weeks=2)

COMMIT_QUEUE_TIMEOUT = timedelta(minutes=40)
DEFAULT_REQUIRED_BUILD_TIMEOUT = timedelta(hours=1, minutes=20)
DEFAULT_NON_REQUIRED_BUILD_TIMEOUT = timedelta(hours=2)
# 2x the longest "run tests" phase for unittests as of c9bf1dbc9cc46e497b2f12b2d6685ef7348b0726,
# which is 5 mins 47 secs, excluding outliers below
UNITTESTS_TIMEOUT = timedelta(minutes=12)


class TimeoutOverride(BaseModel):
    """
    Specification for overriding a task timeout.

    * task: Name of task to overide.
    * exec_timeout: Value to override exec timeout with.
    * idle_timeout: Value to override idle timeout with.
    """

    task: str
    exec_timeout: Optional[int] = None
    idle_timeout: Optional[int] = None

    @classmethod
    def from_seconds(cls, task: str, exec_timeout_secs: Optional[float],
                     idle_timeout_secs: Optional[float]) -> TimeoutOverride:
        """Create an instance of an override from seconds."""
        exec_timeout = exec_timeout_secs / 60 if exec_timeout_secs else None
        idle_timeout = idle_timeout_secs / 60 if idle_timeout_secs else None
        return cls(
            task=task,
            exec_timeout=exec_timeout,
            idle_timeout=idle_timeout,
        )

    def get_exec_timeout(self) -> Optional[timedelta]:
        """Get a timedelta of the exec timeout to use."""
        if self.exec_timeout is not None:
            return timedelta(minutes=self.exec_timeout)
        return None

    def get_idle_timeout(self) -> Optional[timedelta]:
        """Get a timedelta of the idle timeout to use."""
        if self.idle_timeout is not None:
            return timedelta(minutes=self.idle_timeout)
        return None


class TimeoutOverrides(BaseModel):
    """Collection of timeout overrides to apply."""

    overrides: Dict[str, List[TimeoutOverride]]

    @classmethod
    def from_yaml_file(cls, file_path: Path) -> "TimeoutOverrides":
        """Read the timeout overrides from the given file."""
        with open(file_path) as file_handler:
            return cls(**yaml.safe_load(file_handler))

    def _lookup_override(self, build_variant: str, task_name: str) -> Optional[TimeoutOverride]:
        """
        Check if the given task on the given build variant has an override defined.

        Note: If multiple overrides are found, an exception will be raised.

        :param build_variant: Build Variant to check.
        :param task_name: Task name to check.
        :return: Timeout override if found.
        """
        overrides = [
            override for override in self.overrides.get(build_variant, [])
            if override.task == task_name
        ]
        if overrides:
            if len(overrides) > 1:
                LOGGER.error("Found multiple overrides for the same task",
                             build_variant=build_variant, task=task_name,
                             overrides=[override.dict() for override in overrides])
                raise ValueError(f"Found multiple overrides for '{task_name}' on '{build_variant}'")
            return overrides[0]
        return None

    def lookup_exec_override(self, build_variant: str, task_name: str) -> Optional[timedelta]:
        """
        Look up the exec timeout override of the given build variant/task.

        :param build_variant: Build Variant to check.
        :param task_name: Task name to check.
        :return: Exec timeout override if found.
        """
        override = self._lookup_override(build_variant, task_name)
        if override is not None:
            return override.get_exec_timeout()
        return None

    def lookup_idle_override(self, build_variant: str, task_name: str) -> Optional[timedelta]:
        """
        Look up the idle timeout override of the given build variant/task.

        :param build_variant: Build Variant to check.
        :param task_name: Task name to check.
        :return: Idle timeout override if found.
        """
        override = self._lookup_override(build_variant, task_name)
        if override is not None:
            return override.get_idle_timeout()
        return None


def _is_required_build_variant(build_variant: str) -> bool:
    """
    Determine if the given build variants is a required build variant.

    :param build_variant: Name of build variant to check.
    :return: True if the given build variant is required.
    """
    return build_variant.endswith("-required")


def output_timeout(exec_timeout: timedelta, idle_timeout: Optional[timedelta],
                   output_file: Optional[str]) -> None:
    """
    Output timeout configuration to the specified location.

    :param exec_timeout: Exec timeout to output.
    :param idle_timeout: Idle timeout to output.
    :param output_file: Location of output file to write.
    """
    # the math library is triggering this error in this function for some
    # reason
    # pylint: disable=c-extension-no-member
    output = {
        "exec_timeout_secs": math.ceil(exec_timeout.total_seconds()),
    }
    if idle_timeout is not None:
        output["timeout_secs"] = math.ceil(idle_timeout.total_seconds())

    if output_file:
        with open(output_file, "w") as outfile:
            yaml.dump(output, stream=outfile, default_flow_style=False)

    yaml.dump(output, stream=sys.stdout, default_flow_style=False)


class TaskTimeoutOrchestrator:
    """An orchestrator for determining task timeouts."""

    @inject.autoparams()
    def __init__(self, timeout_service: TimeoutService, timeout_overrides: TimeoutOverrides,
                 evg_project_config: EvergreenProjectConfig) -> None:
        """
        Initialize the orchestrator.

        :param timeout_service: Service for calculating historic timeouts.
        :param timeout_overrides: Timeout overrides for specific tasks.
        :param evg_project_config: Evergreen project configuration.
        """
        self.timeout_service = timeout_service
        self.timeout_overrides = timeout_overrides
        self.evg_project_config = evg_project_config

    def determine_exec_timeout(self, task_name: str, variant: str,
                               idle_timeout: Optional[timedelta] = None,
                               exec_timeout: Optional[timedelta] = None, evg_alias: str = "",
                               historic_timeout: Optional[timedelta] = None) -> timedelta:
        """
        Determine what exec timeout should be used.

        :param task_name: Name of task being run.
        :param variant: Name of build variant being run.
        :param idle_timeout: Idle timeout if specified.
        :param exec_timeout: Override to use for exec_timeout or 0 if no override.
        :param evg_alias: Evergreen alias running the task.
        :param historic_timeout: Timeout determined by looking at previous task executions.
        :return: Exec timeout to use for running task.
        """
        determined_timeout = DEFAULT_NON_REQUIRED_BUILD_TIMEOUT
        if historic_timeout is not None:
            determined_timeout = historic_timeout

        override = self.timeout_overrides.lookup_exec_override(variant, task_name)

        if exec_timeout and exec_timeout.total_seconds() != 0:
            LOGGER.info("Using timeout from cmd line",
                        exec_timeout_secs=exec_timeout.total_seconds())
            determined_timeout = exec_timeout

        elif override is not None:
            LOGGER.info("Overriding configured timeout", exec_timeout_secs=override.total_seconds())
            determined_timeout = override

        elif task_name == UNITTEST_TASK and override is None:
            LOGGER.info("Overriding unittest timeout",
                        exec_timeout_secs=UNITTESTS_TIMEOUT.total_seconds())
            determined_timeout = UNITTESTS_TIMEOUT

        elif _is_required_build_variant(
                variant) and determined_timeout > DEFAULT_REQUIRED_BUILD_TIMEOUT:
            LOGGER.info("Overriding required-builder timeout",
                        exec_timeout_secs=DEFAULT_REQUIRED_BUILD_TIMEOUT.total_seconds())
            determined_timeout = DEFAULT_REQUIRED_BUILD_TIMEOUT

        elif evg_alias == COMMIT_QUEUE_ALIAS:
            LOGGER.info("Overriding commit-queue timeout",
                        exec_timeout_secs=COMMIT_QUEUE_TIMEOUT.total_seconds())
            determined_timeout = COMMIT_QUEUE_TIMEOUT

        # The timeout needs to be at least as large as the idle timeout.
        if idle_timeout and determined_timeout.total_seconds() < idle_timeout.total_seconds():
            LOGGER.info("Making exec timeout as large as idle timeout",
                        exec_timeout_secs=idle_timeout.total_seconds())
            return idle_timeout

        return determined_timeout

    def determine_idle_timeout(self, task_name: str, variant: str,
                               idle_timeout: Optional[timedelta] = None,
                               historic_timeout: Optional[timedelta] = None) -> Optional[timedelta]:
        """
        Determine what idle timeout should be used.

        :param task_name: Name of task being run.
        :param variant: Name of build variant being run.
        :param idle_timeout: Override to use for idle_timeout.
        :param historic_timeout: Timeout determined by looking at previous task executions.
        :return: Idle timeout to use for running task.
        """
        determined_timeout = historic_timeout

        override = self.timeout_overrides.lookup_idle_override(variant, task_name)

        if idle_timeout and idle_timeout.total_seconds() != 0:
            LOGGER.info("Using timeout from cmd line",
                        idle_timeout_secs=idle_timeout.total_seconds())
            determined_timeout = idle_timeout

        elif override is not None:
            LOGGER.info("Overriding configured timeout", idle_timeout_secs=override.total_seconds())
            determined_timeout = override

        return determined_timeout

    def determine_historic_timeout(self, task: str, variant: str, suite_name: str,
                                   exec_timeout_factor: Optional[float]) -> TimeoutOverride:
        """
        Calculate the timeout based on historic test results.

        :param task: Name of task to query.
        :param variant: Name of build variant to query.
        :param suite_name: Name of test suite being run.
        :param exec_timeout_factor: Scaling factor to use when determining timeout.
        """
        if suite_name in IGNORED_SUITES:
            return TimeoutOverride(task=task, exec_timeout=None, idle_timeout=None)

        timeout_params = TimeoutParams(
            evg_project="mongodb-mongo-master",
            build_variant=variant,
            task_name=task,
            suite_name=suite_name,
            is_asan=self.is_build_variant_asan(variant),
        )
        timeout_estimate = self.timeout_service.get_timeout_estimate(timeout_params)
        if timeout_estimate and timeout_estimate.is_specified():
            exec_timeout = timeout_estimate.calculate_task_timeout(
                repeat_factor=1, scaling_factor=exec_timeout_factor)
            idle_timeout = timeout_estimate.calculate_test_timeout(repeat_factor=1)
            if exec_timeout is not None or idle_timeout is not None:
                LOGGER.info("Getting historic based timeout", exec_timeout_secs=exec_timeout,
                            idle_timeout_secs=idle_timeout)
                return TimeoutOverride.from_seconds(task, exec_timeout, idle_timeout)
        return TimeoutOverride(task=task, exec_timeout=None, idle_timeout=None)

    def is_build_variant_asan(self, build_variant: str) -> bool:
        """
        Determine if the given build variant is an ASAN build variant.

        :param build_variant: Name of build variant to check.
        :return: True if build variant is an ASAN build variant.
        """
        bv = self.evg_project_config.get_variant(build_variant)
        return bv.is_asan_build()

    def determine_timeouts(self, cli_idle_timeout: Optional[timedelta],
                           cli_exec_timeout: Optional[timedelta], outfile: Optional[str], task: str,
                           variant: str, evg_alias: str, suite_name: str,
                           exec_timeout_factor: Optional[float]) -> None:
        """
        Determine the timeouts to use for the given task and write timeouts to expansion file.

        :param cli_idle_timeout: Idle timeout specified by the CLI.
        :param cli_exec_timeout: Exec timeout specified by the CLI.
        :param outfile: File to write timeout expansions to.
        :param variant: Build variant task is being run on.
        :param evg_alias: Evergreen alias that triggered task.
        :param suite_name: Name of evergreen suite being run.
        :param exec_timeout_factor: Scaling factor to use when determining timeout.
        """
        historic_timeout = self.determine_historic_timeout(task, variant, suite_name,
                                                           exec_timeout_factor)

        idle_timeout = self.determine_idle_timeout(task, variant, cli_idle_timeout,
                                                   historic_timeout.get_idle_timeout())
        exec_timeout = self.determine_exec_timeout(task, variant, idle_timeout, cli_exec_timeout,
                                                   evg_alias, historic_timeout.get_exec_timeout())

        output_timeout(exec_timeout, idle_timeout, outfile)


def main():
    """Determine the timeout value a task should use in evergreen."""
    parser = argparse.ArgumentParser(description=main.__doc__)

    parser.add_argument("--install-dir", dest="install_dir", required=True,
                        help="Path to bin directory of testable installation")
    parser.add_argument("--task-name", dest="task", required=True, help="Task being executed.")
    parser.add_argument("--suite-name", dest="suite_name", required=True,
                        help="Resmoke suite being run against.")
    parser.add_argument("--build-variant", dest="variant", required=True,
                        help="Build variant task is being executed on.")
    parser.add_argument("--evg-alias", dest="evg_alias", required=True,
                        help="Evergreen alias used to trigger build.")
    parser.add_argument("--timeout", dest="timeout", type=int, help="Timeout to use (in sec).")
    parser.add_argument("--exec-timeout", dest="exec_timeout", type=int,
                        help="Exec timeout to use (in sec).")
    parser.add_argument("--exec-timeout-factor", dest="exec_timeout_factor", type=float,
                        help="Exec timeout factor to use (in sec).")
    parser.add_argument("--out-file", dest="outfile", help="File to write configuration to.")
    parser.add_argument("--timeout-overrides", dest="timeout_overrides_file",
                        default=DEFAULT_TIMEOUT_OVERRIDES,
                        help="File containing timeout overrides to use.")
    parser.add_argument("--evg-api-config", dest="evg_api_config",
                        default=DEFAULT_EVERGREEN_AUTH_CONFIG, help="Evergreen API config file.")
    parser.add_argument("--evg-project-config", dest="evg_project_config",
                        default=DEFAULT_EVERGREEN_CONFIG, help="Evergreen project config file.")

    options = parser.parse_args()

    end_date = datetime.now()
    start_date = end_date - HISTORY_LOOKBACK

    timeout_override = timedelta(seconds=options.timeout) if options.timeout else None
    exec_timeout_override = timedelta(
        seconds=options.exec_timeout) if options.exec_timeout else None

    task_name = determine_task_base_name(options.task, options.variant)
    timeout_overrides = TimeoutOverrides.from_yaml_file(
        os.path.expanduser(options.timeout_overrides_file))

    enable_logging(verbose=False)

    def dependencies(binder: inject.Binder) -> None:
        binder.bind(
            EvergreenApi,
            RetryingEvergreenApi.get_api(config_file=os.path.expanduser(options.evg_api_config)))
        binder.bind(TimeoutSettings, TimeoutSettings(start_date=start_date, end_date=end_date))
        binder.bind(TimeoutOverrides, timeout_overrides)
        binder.bind(EvergreenProjectConfig,
                    parse_evergreen_file(os.path.expanduser(options.evg_project_config)))
        binder.bind(
            ResmokeProxyService,
            ResmokeProxyService(run_options=f"--installDir={shlex.quote(options.install_dir)}"))

    inject.configure(dependencies)

    task_timeout_orchestrator = inject.instance(TaskTimeoutOrchestrator)
    task_timeout_orchestrator.determine_timeouts(
        timeout_override, exec_timeout_override, options.outfile, task_name, options.variant,
        options.evg_alias, options.suite_name, options.exec_timeout_factor)


if __name__ == "__main__":
    main()
