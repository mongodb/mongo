#!/usr/bin/env python3
"""Determine the number of resmoke jobs to run."""

import argparse
import logging
import platform
import re
import sys
from collections import defaultdict

import psutil
import structlog
import yaml

LOGGER = structlog.get_logger(__name__)

CPU_COUNT = psutil.cpu_count()
PLATFORM_MACHINE = platform.machine()
SYS_PLATFORM = sys.platform

# The following constants define tasks that should override the resmoke jobs in various
# configurations. The factor value will set the max number of resmoke jobs based on the number
# of CPUs a machine has. For example, if the factor is 0.5 and a machine has 8 CPUs, the max resmoke
# jobs would be 4 (8 * 0.5). If the running task has multiple overrides that apply, the lowest
# value will be used.
#
# The task name is specified as a regex. The task name used will be the task executing the test,
# which means if the task has been split to run in sub-tasks, an extra "_0", "_1", ... will be
# appended to the task name. For this reason, most task names should end with a ".*".

# Apply factor for a task based on the build variant it is running on.
VARIANT_TASK_FACTOR_OVERRIDES = {
    "enterprise-rhel-80-64-bit": [{"task": r"logical_session_cache_replication.*", "factor": 0.75}],
    "enterprise-rhel-80-64-bit-inmem": [
        {"task": "secondary_reads_passthrough", "factor": 0.3},
        {"task": "multi_stmt_txn_jscore_passthrough_with_migration", "factor": 0.3},
    ]
}

TASKS_FACTORS = [{"task": r"replica_sets.*", "factor": 0.5}, {"task": r"sharding.*", "factor": 0.5}]

DISTRO_MULTIPLIERS = {"rhel80-large": 1.618}

# Apply factor for a task based on the machine type it is running on.
MACHINE_TASK_FACTOR_OVERRIDES = {
    "aarch64":
        TASKS_FACTORS,
    "ppc64le": [
        dict(task=r"causally_consistent_hedged_reads_jscore_passthrough.*", factor=0.125),
        dict(task=r"causally_consistent_read_concern_snapshot_passthrough.*", factor=0.125),
        dict(task=r"sharded_causally_consistent_read_concern_snapshot_passthrough.*", factor=0.125),
    ],
}

# Apply factor for a task based on the platform it is running on.
PLATFORM_TASK_FACTOR_OVERRIDES = {"win32": TASKS_FACTORS, "cygwin": TASKS_FACTORS}

# Apply factor for a task everywhere it is run.
GLOBAL_TASK_FACTOR_OVERRIDES = {
    r"causally_consistent_hedged_reads_jscore_passthrough.*": 0.25,
    r"logical_session_cache.*_refresh_jscore_passthrough.*": 0.25,
    r"multi_shard_.*multi_stmt_txn_.*jscore_passthrough.*": 0.125,
    r"replica_sets_reconfig_jscore_passthrough.*": 0.25,
    r"replica_sets_reconfig_jscore_stepdown_passthrough.*": 0.25,
    r"replica_sets_reconfig_kill_primary_jscore_passthrough.*": 0.25,
    r"sharded_causally_consistent_jscore_passthrough.*": 0.75,
    r"sharded_collections_jscore_passthrough.*": 0.75,
}


def global_task_factor(task_name, overrides, factor):
    """
    Check for a global task override and return factor.

    :param task_name: Name of task to check for.
    :param overrides: Global override data.
    :param factor: Default factor if there is no override.
    :return: Factor that should be used based on global overrides.
    """
    for task_re, task_factor in overrides.items():
        if re.compile(task_re).match(task_name):
            return task_factor

    return factor


def get_task_factor(task_name, overrides, override_type, factor):
    """Check for task override and return factor."""
    for task_override in overrides.get(override_type, []):
        if re.compile(task_override["task"]).match(task_name):
            return task_override["factor"]
    return factor


def determine_final_multiplier(distro):
    """Determine the final multiplier."""
    multipliers = defaultdict(lambda: 1, DISTRO_MULTIPLIERS)
    return multipliers[distro]


def determine_factor(task_name, variant, distro, factor):
    """Determine the job factor."""
    factors = [
        get_task_factor(task_name, MACHINE_TASK_FACTOR_OVERRIDES, PLATFORM_MACHINE, factor),
        get_task_factor(task_name, PLATFORM_TASK_FACTOR_OVERRIDES, SYS_PLATFORM, factor),
        get_task_factor(task_name, VARIANT_TASK_FACTOR_OVERRIDES, variant, factor),
        global_task_factor(task_name, GLOBAL_TASK_FACTOR_OVERRIDES, factor),
    ]
    return min(factors) * determine_final_multiplier(distro)


def determine_jobs(task_name, variant, distro, jobs_max=0, job_factor=1.0):
    """Determine the resmoke jobs."""
    if jobs_max < 0:
        raise ValueError("The jobs_max must be >= 0.")
    if job_factor <= 0:
        raise ValueError("The job_factor must be > 0.")
    factor = determine_factor(task_name, variant, distro, job_factor)
    jobs_available = int(round(CPU_COUNT * factor))
    if jobs_max == 0:
        return max(1, jobs_available)
    return min(jobs_max, jobs_available)


def output_jobs(jobs, outfile):
    """Output jobs configuration to the specified location."""
    output = {"resmoke_jobs": jobs}

    if outfile:
        with open(outfile, "w") as fh:
            yaml.dump(output, stream=fh, default_flow_style=False)

    yaml.dump(output, stream=sys.stdout, default_flow_style=False)


def main():
    """Determine the resmoke jobs value a task should use in Evergreen."""
    parser = argparse.ArgumentParser(description=main.__doc__)

    parser.add_argument("--taskName", dest="task", required=True, help="Task being executed.")
    parser.add_argument("--buildVariant", dest="variant", required=True,
                        help="Build variant task is being executed on.")
    parser.add_argument("--distro", dest="distro", required=True,
                        help="Distro task is being executed on.")
    parser.add_argument(
        "--jobFactor", dest="jobs_factor", type=float, default=1.0,
        help=("Job factor to use as a mulitplier with the number of CPUs. Defaults"
              " to %(default)s."))
    parser.add_argument(
        "--jobsMax", dest="jobs_max", type=int, default=0,
        help=("Maximum number of jobs to use. Specify 0 to indicate the number of"
              " jobs is determined by --jobFactor and the number of CPUs. Defaults"
              " to %(default)s."))
    parser.add_argument(
        "--outFile", dest="outfile", help=("File to write configuration to. If"
                                           " unspecified no file is generated."))

    options = parser.parse_args()

    logging.basicConfig(stream=sys.stdout, level=logging.INFO)
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())

    LOGGER.info("Finding job count", task=options.task, variant=options.variant,
                platform=PLATFORM_MACHINE, sys=SYS_PLATFORM, cpu_count=CPU_COUNT)

    jobs = determine_jobs(options.task, options.variant, options.distro, options.jobs_max,
                          options.jobs_factor)
    if jobs < CPU_COUNT:
        print("Reducing number of jobs to run from {} to {}".format(CPU_COUNT, jobs))
    output_jobs(jobs, options.outfile)


if __name__ == "__main__":
    main()
