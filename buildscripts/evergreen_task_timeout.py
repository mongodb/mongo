#!/usr/bin/env python3
"""Determine the timeout value a task should use in evergreen."""

import argparse
import math
import sys
from datetime import timedelta
from typing import Optional

import yaml

COMMIT_QUEUE_ALIAS = "__commit_queue"
UNITTEST_TASK = "run_unittests"

COMMIT_QUEUE_TIMEOUT = timedelta(minutes=40)
DEFAULT_REQUIRED_BUILD_TIMEOUT = timedelta(hours=1, minutes=20)
DEFAULT_NON_REQUIRED_BUILD_TIMEOUT = timedelta(hours=2)
# 2x the longest "run tests" phase for unittests as of c9bf1dbc9cc46e497b2f12b2d6685ef7348b0726,
# which is 5 mins 47 secs, excluding outliers below
UNITTESTS_TIMEOUT = timedelta(minutes=12)

SPECIFIC_TASK_OVERRIDES = {
    "linux-64-debug": {"auth": timedelta(minutes=60)},
    "enterprise-windows-all-feature-flags-suggested": {
        "replica_sets_jscore_passthrough": timedelta(hours=3),
        "replica_sets_update_v1_oplog_jscore_passthrough": timedelta(hours=2, minutes=30),
    },
    "enterprise-windows-suggested": {
        "replica_sets_jscore_passthrough": timedelta(hours=3),
        "replica_sets_update_v1_oplog_jscore_passthrough": timedelta(hours=2, minutes=30),
    },
    "enterprise-windows-inmem": {"replica_sets_jscore_passthrough": timedelta(hours=3), },
    "enterprise-windows": {"replica_sets_jscore_passthrough": timedelta(hours=3), },
    "windows-debug-suggested": {
        "replica_sets_initsync_jscore_passthrough": timedelta(hours=2, minutes=30),
        "replica_sets_jscore_passthrough": timedelta(hours=2, minutes=30),
        "replica_sets_update_v1_oplog_jscore_passthrough": timedelta(hours=2, minutes=30),
    },
    "windows": {
        "replica_sets": timedelta(hours=3),
        "replica_sets_jscore_passthrough": timedelta(hours=2, minutes=30),
    },
    "ubuntu1804-debug-suggested": {"replica_sets_jscore_passthrough": timedelta(hours=3), },
    "enterprise-rhel-80-64-bit-coverage": {
        "replica_sets_jscore_passthrough": timedelta(hours=2, minutes=30),
    },
    "macos": {"replica_sets_jscore_passthrough": timedelta(hours=2, minutes=30), },
    "enterprise-macos": {"replica_sets_jscore_passthrough": timedelta(hours=2, minutes=30), },

    # unittests outliers
    # repeated execution runs a suite 10 times
    "linux-64-repeated-execution": {UNITTEST_TASK: 10 * UNITTESTS_TIMEOUT},
    # some of the a/ub/t san variants need a little extra time
    "enterprise-ubuntu2004-debug-tsan": {UNITTEST_TASK: 2 * UNITTESTS_TIMEOUT},
    "ubuntu1804-asan": {UNITTEST_TASK: 2 * UNITTESTS_TIMEOUT},
    "ubuntu1804-ubsan": {UNITTEST_TASK: 2 * UNITTESTS_TIMEOUT},
    "ubuntu1804-debug-asan": {UNITTEST_TASK: 2 * UNITTESTS_TIMEOUT},
    "ubuntu1804-debug-aubsan-lite": {UNITTEST_TASK: 2 * UNITTESTS_TIMEOUT},
    "ubuntu1804-debug-ubsan": {UNITTEST_TASK: 2 * UNITTESTS_TIMEOUT},
}


def _is_required_build_variant(build_variant: str) -> bool:
    """
    Determine if the given build variants is a required build variant.

    :param build_variant: Name of build variant to check.
    :return: True if the given build variant is required.
    """
    return build_variant.endswith("-required")


def _has_override(variant: str, task_name: str) -> bool:
    """
    Determine if the given task has a timeout override.

    :param variant: Build Variant task is running on.
    :param task_name: Task to check.
    :return: True if override exists for task.
    """
    return variant in SPECIFIC_TASK_OVERRIDES and task_name in SPECIFIC_TASK_OVERRIDES[variant]


def determine_timeout(task_name: str, variant: str, idle_timeout: Optional[timedelta] = None,
                      exec_timeout: Optional[timedelta] = None, evg_alias: str = '') -> timedelta:
    """
    Determine what exec timeout should be used.

    :param task_name: Name of task being run.
    :param variant: Name of build variant being run.
    :param idle_timeout: Idle timeout if specified.
    :param exec_timeout: Override to use for exec_timeout or 0 if no override.
    :param evg_alias: Evergreen alias running the task.
    :return: Exec timeout to use for running task.
    """
    determined_timeout = DEFAULT_NON_REQUIRED_BUILD_TIMEOUT

    if exec_timeout and exec_timeout.total_seconds() != 0:
        determined_timeout = exec_timeout

    elif task_name == UNITTEST_TASK and not _has_override(variant, task_name):
        determined_timeout = UNITTESTS_TIMEOUT

    elif evg_alias == COMMIT_QUEUE_ALIAS:
        determined_timeout = COMMIT_QUEUE_TIMEOUT

    elif _has_override(variant, task_name):
        determined_timeout = SPECIFIC_TASK_OVERRIDES[variant][task_name]

    elif _is_required_build_variant(variant):
        determined_timeout = DEFAULT_REQUIRED_BUILD_TIMEOUT

    # The timeout needs to be at least as large as the idle timeout.
    if idle_timeout and determined_timeout.total_seconds() < idle_timeout.total_seconds():
        return idle_timeout

    return determined_timeout


def output_timeout(task_timeout: timedelta, output_file: Optional[str]) -> None:
    """
    Output timeout configuration to the specified location.

    :param task_timeout: Timeout to output.
    :param output_file: Location of output file to write.
    """
    output = {
        "exec_timeout_secs": math.ceil(task_timeout.total_seconds()),
    }

    if output_file:
        with open(output_file, "w") as outfile:
            yaml.dump(output, stream=outfile, default_flow_style=False)

    yaml.dump(output, stream=sys.stdout, default_flow_style=False)


def main():
    """Determine the timeout value a task should use in evergreen."""
    parser = argparse.ArgumentParser(description=main.__doc__)

    parser.add_argument("--task-name", dest="task", required=True, help="Task being executed.")
    parser.add_argument("--build-variant", dest="variant", required=True,
                        help="Build variant task is being executed on.")
    parser.add_argument("--evg-alias", dest="evg_alias", required=True,
                        help="Evergreen alias used to trigger build.")
    parser.add_argument("--timeout", dest="timeout", type=int, help="Timeout to use (in sec).")
    parser.add_argument("--exec-timeout", dest="exec_timeout", type=int,
                        help="Exec timeout ot use (in sec).")
    parser.add_argument("--out-file", dest="outfile", help="File to write configuration to.")

    options = parser.parse_args()

    timeout_override = timedelta(seconds=options.timeout) if options.timeout else None
    exec_timeout_override = timedelta(
        seconds=options.exec_timeout) if options.exec_timeout else None
    task_timeout = determine_timeout(options.task, options.variant, timeout_override,
                                     exec_timeout_override, options.evg_alias)
    output_timeout(task_timeout, options.outfile)


if __name__ == "__main__":
    main()
