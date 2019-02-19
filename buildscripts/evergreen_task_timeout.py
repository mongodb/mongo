#!/usr/bin/env python
"""Determine the timeout value a task should use in evergreen."""

import argparse
import sys

import yaml

DEFAULT_REQUIRED_BUILD_TIMEOUT_SECS = 30 * 60
DEFAULT_NON_REQUIRED_BUILD_TIMEOUT_SECS = 2 * 60 * 60

SPECIFIC_TASK_OVERRIDES = {
    "linux-64-debug": {"auth": 60 * 60, },
}

REQUIRED_BUILD_VARIANTS = {
    "linux-64-debug", "enterprise-windows-64-2k8", "enterprise-rhel-62-64-bit",
    "enterprise-ubuntu1604-clang-3.8-libcxx", "enterprise-rhel-62-64-bit-required-inmem",
    "rhel-62-64-bit-required-mobile", "ubuntu1604-debug-aubsan-lite"
}


def determine_timeout(task_name, variant, timeout=0):
    """Determine what timeout should be used."""

    if timeout and timeout != 0:
        return timeout

    if variant in SPECIFIC_TASK_OVERRIDES and task_name in SPECIFIC_TASK_OVERRIDES[variant]:
        return SPECIFIC_TASK_OVERRIDES[variant][task_name]

    if variant in REQUIRED_BUILD_VARIANTS:
        return DEFAULT_REQUIRED_BUILD_TIMEOUT_SECS
    return DEFAULT_NON_REQUIRED_BUILD_TIMEOUT_SECS


def output_timeout(timeout, options):
    """Output timeout configuration to the specified location."""
    output = {
        "timeout_secs": timeout,
    }

    if options.outfile:
        with open(options.outfile, "w") as outfile:
            yaml.dump(output, stream=outfile, default_flow_style=False)

    yaml.dump(output, stream=sys.stdout, default_flow_style=False)


def main():
    """Determine the timeout value a task should use in evergreen."""
    parser = argparse.ArgumentParser(description=main.__doc__)

    parser.add_argument("--task-name", dest="task", required=True, help="Task being executed.")
    parser.add_argument("--build-variant", dest="variant", required=True,
                        help="Build variant task is being executed on.")
    parser.add_argument("--timeout", dest="timeout", type=int, help="Timeout to use.")
    parser.add_argument("--out-file", dest="outfile", help="File to write configuration to.")

    options = parser.parse_args()

    timeout = determine_timeout(options.task, options.variant, options.timeout)
    output_timeout(timeout, options)


if __name__ == "__main__":
    main()
