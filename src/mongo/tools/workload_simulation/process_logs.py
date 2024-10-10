#!/usr/bin/env python3
import argparse
import json
import sys

import matplotlib.pyplot as pyplot

LOG_ID_WORKLOAD_NAME = 7782100
LOG_ID_METRICS = 7782101

parser = argparse.ArgumentParser(description="Process simulator log output piped to stdin")
parser.add_argument(
    "-o",
    "--outputDirectory",
    help="Path to an existing directory where output should be stored",
    metavar="~/path/for/output",
)
args = parser.parse_args()

directory = args.outputDirectory

# Load the log data that's piped in.
workloads = {}
currentWorkload = ""
firstTime = 0
for line in sys.stdin:
    parsed = json.loads(line)
    if parsed["id"] == LOG_ID_WORKLOAD_NAME:
        # Starting output for a new workload.
        currentWorkload = parsed["attr"]["workload"]
        # Can't reuse the workload name
        assert currentWorkload not in workloads
        # Initialize the data structure for the output.
        workloads[currentWorkload] = {"time": [], "metrics": {}}

    elif parsed["id"] == LOG_ID_METRICS:
        # Check that data structure for current workload has been initialized properly.
        assert isinstance(workloads[currentWorkload], dict)
        assert isinstance(workloads[currentWorkload]["time"], list)
        assert isinstance(workloads[currentWorkload]["metrics"], dict)

        # Parsing output for the current workload.

        # Normalize time values so that first time value in series is '0', displayed in seconds.
        if len(workloads[currentWorkload]["time"]) == 0:
            firstTime = parsed["attr"]["time"]
        workloads[currentWorkload]["time"].append((parsed["attr"]["time"] - firstTime) / 1e9)

        # Process the metrics, initializing structures as necessary
        metrics = parsed["attr"]["metrics"]
        for grouping, data in metrics.items():
            if grouping not in workloads[currentWorkload]["metrics"]:
                workloads[currentWorkload]["metrics"][grouping] = {}
            for key, value in data.items():
                if key not in workloads[currentWorkload]["metrics"][grouping]:
                    workloads[currentWorkload]["metrics"][grouping][key] = []
                workloads[currentWorkload]["metrics"][grouping][key].append(value)

# Plot the data and save the resulting figures to the output directory.
for workload, data in workloads.items():
    numPlots = len(data["metrics"])
    width = min(numPlots, 3)
    height = int(numPlots / width) + (numPlots % width > 0)

    fig, ax = pyplot.subplots(
        nrows=height,
        ncols=width,
        figsize=(7.5 * width, 3.5 * height),
        sharex=True,
        layout="constrained",
    )
    fig.suptitle("Workload: " + workload)

    i = 0
    for grouping, metrics in data["metrics"].items():
        ax[i].set_title(grouping)
        ax[i].set_xlabel("Time (s)")
        for key, values in metrics.items():
            ax[i].plot(data["time"], values, label=key)
        ax[i].legend()

        i = i + 1

    fig.savefig(directory + "/" + workload + ".png", dpi=300)
