#!/usr/bin/env python3
"""Make SCons metrics cedar report."""
import json
import os.path
import sys

import click

from buildscripts.scons_metrics.metrics import SconsMetrics

SCONS_STDOUT_LOG = "scons_stdout.log"
SCONS_CACHE_DEBUG_LOG = "scons_cache.log"
CEDAR_REPORT_FILE = "scons_cedar_report.json"


@click.command()
@click.option("--scons-stdout-log-file", default=SCONS_STDOUT_LOG, type=str,
              help="Path to the file with SCons stdout logs.")
@click.option("--scons-cache-debug-log-file", default=SCONS_CACHE_DEBUG_LOG, type=str,
              help="Path to the file with SCons stdout logs.")
@click.option("--cedar-report-file", default=CEDAR_REPORT_FILE, type=str,
              help="Path to cedar report json file.")
def main(scons_stdout_log_file: str, scons_cache_debug_log_file: str,
         cedar_report_file: str) -> None:
    """Read SCons stdout log file and write cedar report json file."""
    scons_stdout_log_file = os.path.abspath(scons_stdout_log_file)
    scons_cache_debug_log_file = os.path.abspath(scons_cache_debug_log_file)
    cedar_report_file = os.path.abspath(cedar_report_file)

    if not os.path.exists(scons_stdout_log_file):
        print(f"Could not find SCons stdout log file '{scons_stdout_log_file}'.")
        sys.exit(1)

    scons_metrics = SconsMetrics(scons_stdout_log_file, scons_cache_debug_log_file)
    if not scons_metrics.raw_report:
        print(
            f"Could not find raw metrics data in SCons stdout log file '{scons_stdout_log_file}'.")
        sys.exit(1)

    cedar_report = scons_metrics.make_cedar_report()
    with open(cedar_report_file, "w") as fh:
        json.dump(cedar_report, fh)
        print(f"Done dumping cedar report json to file '{cedar_report_file}'.")


if __name__ == '__main__':
    main()  # pylint: disable=no-value-for-parameter
