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
    "enterprise-rhel-8-64-bit": [{"task": r"logical_session_cache_replication.*", "factor": 0.75}],
    "enterprise-rhel-8-64-bit-inmem": [
        {"task": "secondary_reads_passthrough", "factor": 0.3},
        {"task": "multi_stmt_txn_jscore_passthrough_with_migration", "factor": 0.3},
    ],
    "enterprise-rhel8-debug-tsan": [
        # Lower the default resmoke_jobs_factor for TSAN to reduce memory pressure for this suite,
        # as otherwise TSAN variants occasionally run out of memory
        # Non-TSAN variants don't need this adjustment as they have a reasonable free memory margin
        {"task": r"fcv_upgrade_downgrade_sharded_collections_jscore_passthrough.*", "factor": 0.27},
        {"task": r"shard.*uninitialized_fcv_jscore_passthrough.*", "factor": 0.125},
    ],
    "enterprise-rhel8-debug-tsan-all-feature-flags": [
        # Lower the default resmoke_jobs_factor for TSAN to reduce memory pressure for this suite,
        # as otherwise TSAN variants occasionally run out of memory.
        # The all feature flags variant sometimes needs more aggressive reductions than the no
        # feature flags variant.
        {
            "task": r"fcv_upgrade_downgrade_sharded_collections_jscore_passthrough.*",
            "factor": 0.125,
        },
        {"task": r"fcv_upgrade_downgrade_replica_sets_jscore_passthrough.*", "factor": 0.27},
        {"task": r"shard.*uninitialized_fcv_jscore_passthrough.*", "factor": 0.125},
    ],
    "rhel8-debug-aubsan-classic-engine": [
        {"task": r"shard.*uninitialized_fcv_jscore_passthrough.*", "factor": 0.25}
    ],
    "rhel8-debug-aubsan-all-feature-flags": [
        {"task": r"shard.*uninitialized_fcv_jscore_passthrough.*", "factor": 0.25}
    ],
    "enterprise-windows-all-feature-flags-required": [{"task": "noPassthrough", "factor": 0.5}],
    "enterprise-windows-all-feature-flags-non-essential": [
        {"task": "noPassthrough", "factor": 0.5}
    ],
    "enterprise-windows": [{"task": "noPassthrough", "factor": 0.5}],
    "windows-debug-suggested": [{"task": "noPassthrough", "factor": 0.5}],
    "windows": [{"task": "noPassthrough", "factor": 0.5}],
}

TASKS_FACTORS = [{"task": r"replica_sets.*", "factor": 0.5}, {"task": r"sharding.*", "factor": 0.5}]

DISTRO_MULTIPLIERS = {"rhel8.8-large": 1.618}

# Apply factor for a task based on the machine type it is running on.
MACHINE_TASK_FACTOR_OVERRIDES = {
    "aarch64": TASKS_FACTORS,
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
    r"shard.*uninitialized_fcv_jscore_passthrough.*": 0.25,
}


def get_original_task_name(task_name):
    """
    The task name after going through the task generator may have the form
    /<task name>_[0-9]+-<platform>/. This function returns the original task
    name.

    For example, "sharding_0-linux-debug" -> "sharding".
    """
    return re.compile("_[0-9]+").split(task_name)[0]


def global_task_factor(generated_task_name, overrides, factor):
    """
    Check for a global task override and return factor.

    :param task_name: Name of task to check for.
    :param overrides: Global override data.
    :param factor: Default factor if there is no override.
    :return: Factor that should be used based on global overrides.
    """
    task_name = get_original_task_name(generated_task_name)
    for task_re, task_factor in overrides.items():
        if re.compile(task_re).search(task_name):
            return task_factor

    return factor


def get_task_factor(generated_task_name, overrides, override_type, factor):
    """Check for task override and return factor."""
    task_name = get_original_task_name(generated_task_name)
    for task_override in overrides.get(override_type, []):
        if re.compile(task_override["task"]).search(task_name):
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


# TODO (SERVER-97801): Delete this function.
def maybe_override_num_jobs_on_required(task_name, variant, jobs):
    """
    Currently we are under-utilizing hosts for certain tasks because
    we're picking a Resmoke job value that's too low. This function
    exists to increase the number of jobs used for certain tasks on
    certain build variants in an effort to speed up patch builds.

    If the task or variant isn't optimizable, this function does nothing.

    :param task_name: Name of task to attempt to optimize.
    :param variant: Name of the build variant.
    :param jobs: The current number of jobs to be used.
    :return: A new number of jobs to be used.
    """
    all_factors = {
        "^replica_sets(_last_lts|_last_continuous)?$": 2,
        "^sharding$": 1.5,
        "^sharding(_last_lts|_last_continuous)$": 2,
        "concurrency_replication": 0.5,
        "concurrency_sharded": 0.25,
        "concurrency.*simultaneous": 0.5,
        "replica_sets.*passthrough": 1,
        "sharded_collections.*with_config_transitions": 2,
        "^sharding_.*jscore_passthrough$": 1,
        "^sharding_.*passthrough.*with_config_transitions": 2,
        "sharding_jscore_passthrough_with_balancer": 1,
        "^search(_auth)?$": 0.5,
        "fuzzer.*deterministic": 1,
        "aggregation_secondary_reads": 0.5,
        "^disk_wiredtiger$": 0.5,
        "multi_shard_local_read_write_multi_stmt_txn_jscore_passthrough": 1,
        "multi_shard_multi_stmt_txn_": 0.5,
        "resharding_timeseries_fuzzer": 0.5,
        "unsplittable_collections_created_on_any_shard_jscore_passthrough": 0.5,
    }

    if (
        variant != "enterprise-amazon-linux2-arm64-all-feature-flags"
        and variant != "linux-64-debug-required"
    ):
        LOGGER.info(f"Variant '{variant}' cannot have its jobs increased. Keeping {jobs} jobs.")
        return jobs
    elif variant == "linux-64-debug-required":
        all_factors |= {
            "^noPassthrough$": 0.5,
            "read_concern_linearizable_passthrough": 1,
            "sharded_collections_uninitialized_fcv_jscore_passthrough": 0.5,
            "sharding_csrs_continuous_config_stepdown": 1,
        }
    else:
        all_factors |= {
            "search_no_pinned_connections_auth": 0.5,
        }

    factor = global_task_factor(task_name, all_factors, -1)

    if factor < 0:
        LOGGER.info(f"Task '{task_name}' was not found in factor list. Keeping {jobs} jobs.")
        return jobs

    jobs = int(round(factor * CPU_COUNT))
    LOGGER.info(f"Task '{task_name}' has factor {factor}; will use {jobs} jobs.")
    return jobs


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
    parser.add_argument(
        "--buildVariant",
        dest="variant",
        required=True,
        help="Build variant task is being executed on.",
    )
    parser.add_argument(
        "--distro", dest="distro", required=True, help="Distro task is being executed on."
    )
    parser.add_argument(
        "--jobFactor",
        dest="jobs_factor",
        type=float,
        default=1.0,
        help=(
            "Job factor to use as a mulitplier with the number of CPUs. Defaults" " to %(default)s."
        ),
    )
    parser.add_argument(
        "--jobsMax",
        dest="jobs_max",
        type=int,
        default=0,
        help=(
            "Maximum number of jobs to use. Specify 0 to indicate the number of"
            " jobs is determined by --jobFactor and the number of CPUs. Defaults"
            " to %(default)s."
        ),
    )
    parser.add_argument(
        "--outFile",
        dest="outfile",
        help=("File to write configuration to. If" " unspecified no file is generated."),
    )

    options = parser.parse_args()

    logging.basicConfig(stream=sys.stdout, level=logging.INFO)
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())

    LOGGER.info(
        "Finding job count",
        options=options,
        platform=PLATFORM_MACHINE,
        sys=SYS_PLATFORM,
        cpu_count=CPU_COUNT,
    )

    jobs = determine_jobs(
        options.task, options.variant, options.distro, options.jobs_max, options.jobs_factor
    )
    jobs = maybe_override_num_jobs_on_required(options.task, options.variant, jobs)

    if jobs < CPU_COUNT:
        print("Reducing number of jobs to run from {} to {}".format(CPU_COUNT, jobs))
    output_jobs(jobs, options.outfile)


if __name__ == "__main__":
    main()
