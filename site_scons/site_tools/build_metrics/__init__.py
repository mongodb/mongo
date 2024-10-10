# Copyright 2020 MongoDB Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
"""Configure the build to track build performance."""

import atexit
import json
import os
import sys
import time
from timeit import default_timer as timer

import psutil
from jsonschema import validate

from .artifacts import CollectArtifacts
from .cache_dir import CacheDirCollector, CacheDirValidateWithMetrics
from .libdeps import LibdepsCollector
from .memory import MemoryMonitor
from .per_action_metrics import PerActionMetrics
from .scons import SConsStats
from .util import CaptureAtexits, add_meta_data, get_build_metric_dict

_SEC_TO_NANOSEC_FACTOR = 1000000000.0
_METRICS_COLLECTORS = []


def finalize_build_metrics(env):
    metrics = get_build_metric_dict()
    metrics["end_time"] = time.time_ns()
    for m in _METRICS_COLLECTORS:
        start_time = timer()
        sys.stdout.write(f"Processing {m.get_name()}...")
        sys.stdout.flush()
        key, value = m.finalize()
        sys.stdout.write(f" {round(timer() - start_time, 2)}s\n")
        metrics[key] = value

    with open(os.path.join(os.path.dirname(__file__), "build_metrics_format.schema")) as f:
        validate(metrics, json.load(f))

    build_metrics_file = env.GetOption("build-metrics")
    if build_metrics_file == "-":
        json.dump(metrics, sys.stdout, indent=4, sort_keys=True)
    else:
        with open(build_metrics_file, "w") as f:
            json.dump(metrics, f, indent=4, sort_keys=True)
        with open(f"{os.path.splitext(build_metrics_file)[0]}-chrome-tracer.json", "w") as f:
            json.dump(generate_chrome_tracer_json(metrics), f, indent=4)


def generate_chrome_tracer_json(metrics):
    tracer_json = {"traceEvents": []}
    job_slots = []
    task_stack = sorted(metrics["build_tasks"], reverse=True, key=lambda x: x["start_time"])

    # Chrome trace organizes tasks per pids, so if we want to have a clean layout which
    # clearly shows concurrent processes, we are creating job slots by comparing start and
    # end times, and using "pid" as the job slot identifier. job_slots are a list of chronologically
    # in order tasks. We keep a list of job slots and always check at the end of the job slot to
    # compare the lowest end time that will accommodate the next task start time. If there are no
    # job slots which can accommodate the next task, we create a new job slot. Note the job slots
    # ordering is similar to how the OS process scheduler would organize and start the processes
    # from the build, however we are reproducing this retroactively and simplistically and it
    # is not guaranteed to match exactly.
    while task_stack:
        task = task_stack.pop()
        candidates = [
            job_slot for job_slot in job_slots if job_slot[-1]["end_time"] < task["start_time"]
        ]
        if candidates:
            # We need to find the best job_slot to add this next task too, so we look at the
            # end_times, the one with the lowest would have been the first one available. We just
            # arbitrarily guess the first one will be the best, then iterate to find out which
            # one is the best. We then add to the existing job_slot which best_candidate points to.
            min_end = candidates[0][-1]["end_time"]
            best_candidate = candidates[0]
            for candidate in candidates:
                if candidate[-1]["end_time"] < min_end:
                    best_candidate = candidate
                    min_end = candidate[-1]["end_time"]

            best_candidate.append(task)
        else:
            # None of the current job slots were available to accommodate the new task so we
            # make a new one.
            job_slots.append([task])

    for i, job_slot in enumerate(job_slots):
        for build_task in job_slot:
            tracer_json["traceEvents"].append(
                {
                    "name": build_task["outputs"][0]
                    if build_task["outputs"]
                    else build_task["builder"],
                    "cat": build_task["builder"],
                    "ph": "X",
                    "ts": build_task["start_time"] / 1000.0,
                    "dur": (build_task["end_time"] - build_task["start_time"]) / 1000.0,
                    "pid": i,
                    "args": {
                        "cpu": build_task["cpu_time"],
                        "mem": build_task["mem_usage"],
                    },
                }
            )

    return tracer_json


def generate(env, **kwargs):
    global _METRICS_COLLECTORS

    # This will force our at exit to the of the stack ensuring
    # that it is the last thing called when exiting.
    c = CaptureAtexits()
    atexit.unregister(c)
    for func in c.captured:
        atexit.unregister(func)
    atexit.register(finalize_build_metrics, env)
    for func in c.captured:
        atexit.register(func)

    env.AddMethod(get_build_metric_dict, "GetBuildMetricDictionary")
    env.AddMethod(add_meta_data, "AddBuildMetricsMetaData")

    metrics = get_build_metric_dict()
    p = psutil.Process(os.getpid())

    metrics["start_time"] = int(p.create_time() * _SEC_TO_NANOSEC_FACTOR)
    metrics["scons_command"] = " ".join([sys.executable] + sys.argv)

    _METRICS_COLLECTORS = [
        MemoryMonitor(psutil.Process().memory_info().vms),
        PerActionMetrics(),
        CollectArtifacts(env),
        SConsStats(),
        CacheDirCollector(),
        LibdepsCollector(env),
    ]

    env["CACHEDIR_CLASS"] = CacheDirValidateWithMetrics


def exists(env):
    return True


def options(opts):
    """
    Add command line Variables for build metrics tool.
    """
    opts.AddVariables(
        ("BUILD_METRICS_ARTIFACTS_DIR", "Path to scan for artifacts after the build has stopped."),
        ("BUILD_METRICS_BLOATY", "Path to the bloaty bin"),
    )
