#!/usr/bin/env python3
"""Determine the number of resmoke jobs to run."""

import argparse
import platform
import re
import sys

import psutil
import yaml

CPU_COUNT = psutil.cpu_count()
PLATFORM_MACHINE = platform.machine()
SYS_PLATFORM = sys.platform

VARIANT_TASK_FACTOR_OVERRIDES = {
    "enterprise-rhel-62-64-bit-inmem": [{"task": "secondary_reads_passthrough", "factor": 0.3}]
}

TASKS_FACTORS = [{"task": "replica_sets*", "factor": 0.5}, {"task": "sharding.*", "factor": 0.5}]

MACHINE_TASK_FACTOR_OVERRIDES = {"aarch64": TASKS_FACTORS}

PLATFORM_TASK_FACTOR_OVERRIDES = {"win32": TASKS_FACTORS, "cygwin": TASKS_FACTORS}


def get_task_factor(task_name, overrides, override_type, factor):
    """Check for task override and return factor."""
    for task_override in overrides.get(override_type, []):
        if re.compile(task_override["task"]).match(task_name):
            return task_override["factor"]
    return factor


def determine_factor(task_name, variant, factor):
    """Determine the job factor."""
    factors = []
    factors.append(
        get_task_factor(task_name, MACHINE_TASK_FACTOR_OVERRIDES, PLATFORM_MACHINE, factor))
    factors.append(get_task_factor(task_name, PLATFORM_TASK_FACTOR_OVERRIDES, SYS_PLATFORM, factor))
    factors.append(get_task_factor(task_name, VARIANT_TASK_FACTOR_OVERRIDES, variant, factor))
    return min(factors)


def determine_jobs(task_name, variant, jobs_max=0, job_factor=1.0):
    """Determine the resmoke jobs."""
    if jobs_max < 0:
        raise ValueError("The jobs_max must be >= 0.")
    if job_factor <= 0:
        raise ValueError("The job_factor must be > 0.")
    factor = determine_factor(task_name, variant, job_factor)
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

    jobs = determine_jobs(options.task, options.variant, options.jobs_max, options.jobs_factor)
    if jobs < CPU_COUNT:
        print("Reducing number of jobs to run from {} to {}".format(CPU_COUNT, jobs))
    output_jobs(jobs, options.outfile)


if __name__ == "__main__":
    main()
