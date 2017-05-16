"""
Manages interactions with the report.json file.
"""

from __future__ import absolute_import

import json

from . import config
from .testing import report as _report


def write(suites):
    """
    Writes the combined report of all executions if --reportFile was
    specified on the command line.
    """

    if config.REPORT_FILE is None:
        return

    reports = []
    for suite in suites:
        for group in suite.test_groups:
            reports.extend(group.get_reports())

    combined_report_dict = _report.TestReport.combine(*reports).as_dict()
    with open(config.REPORT_FILE, "w") as fp:
        json.dump(combined_report_dict, fp)
