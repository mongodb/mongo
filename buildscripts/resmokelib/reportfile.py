"""Manage interactions with the report.json file."""

import json

from buildscripts.resmokelib import config
from buildscripts.resmokelib.testing import report as _report


def write(suites):
    """Write the combined report of all executions if --reportFile was specified."""

    if config.REPORT_FILE is None:
        return

    reports = []
    for suite in suites:
        reports.extend(suite.get_reports())

    combined_report_dict = _report.TestReport.combine(*reports).as_dict()

    for suite in suites:
        for test_name in getattr(suite, "_tss_skipped_tests", []):
            combined_report_dict["results"].append(
                {
                    "test_file": test_name,
                    "status": "skip",
                    "exit_code": 0,
                    "start": 0,
                    "end": 0,
                    "elapsed": 0,
                    "log_info": {},
                }
            )

    with open(config.REPORT_FILE, "w") as fp:
        json.dump(combined_report_dict, fp)
