#!/usr/bin/env python3
"""Combine JSON report files used in Evergreen."""

import errno
import json
import os
import re
import sys
from optparse import OptionParser

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing import report


def read_json_file(json_file):
    """Read JSON file."""
    with open(json_file) as json_data:
        return json.load(json_data)


def report_exit(combined_test_report):
    """Return report exit code.

    The exit code of this script is based on the following:
        0:  All tests have status "pass".
        31: At least one test has status "fail" or "timeout".
    Note: A test can be considered dynamic if its name contains a ":" character.
    """

    ret = 0
    for test in combined_test_report.test_infos:
        if test.status in ["fail", "timeout"]:
            return 31
    return ret


def check_error(input_count, output_count):
    """Raise error if both input and output exist, or if neither exist."""
    if (not input_count) and (not output_count):
        raise ValueError("None of the input file(s) or output file exists")

    if input_count and output_count:
        raise ValueError("Both input file and output files exist")


def add_bazel_target_info(test_report, report_file):
    outputs_path = os.path.dirname(
        os.path.dirname(os.path.relpath(report_file, start="bazel-testlogs"))
    )
    if re.search(r"shard_\d+_of_\d+", os.path.basename(outputs_path)):
        target_path = os.path.dirname(outputs_path)
    else:
        target_path = outputs_path
    target = "//" + ":".join(target_path.rsplit("/", 1))
    target_string = target.replace("/", "_").replace(":", "_")
    for test in test_report.test_infos:
        test.test_file = f"{target} - {test.test_file}"
        test.group_id = f"{target_string}_{test.group_id}"
        test.log_info["log_name"] = os.path.join(outputs_path, test.log_info["log_name"])
        test.log_info["logs_to_merge"] = [
            os.path.join(outputs_path, log) for log in test.log_info["logs_to_merge"]
        ]


def main():
    """Execute Main program."""
    usage = "usage: %prog [options] report1.json report2.json ..."
    parser = OptionParser(description=__doc__, usage=usage)
    parser.add_option(
        "-o",
        "--output-file",
        dest="outfile",
        default="-",
        help=(
            "If '-', then the combined report file is written to stdout."
            " Any other value is treated as the output file name. By default,"
            " output is written to stdout."
        ),
    )
    parser.add_option(
        "-x",
        "--no-report-exit",
        dest="report_exit",
        default=True,
        action="store_false",
        help="Do not exit with a non-zero code if any test in the report fails.",
    )
    parser.add_option(
        "--add-bazel-target-info",
        dest="add_bazel_target_info",
        default=False,
        action="store_true",
        help="Add bazel targets to the test names and log locations.",
    )

    (options, args) = parser.parse_args()

    if not args:
        sys.exit("No report files were specified")

    report_files = args
    report_files_count = len(report_files)
    test_reports = []

    for report_file in report_files:
        try:
            report_file_json = read_json_file(report_file)
            test_report = report.TestReport.from_dict(report_file_json)
            if options.add_bazel_target_info:
                add_bazel_target_info(test_report, report_file)
            test_reports.append(test_report)
        except IOError as err:
            # errno.ENOENT is the error code for "No such file or directory".
            if err.errno == errno.ENOENT:
                report_files_count -= 1
                continue
            raise

    combined_test_report = report.TestReport.combine(*test_reports)
    combined_report = combined_test_report.as_dict()

    if options.outfile == "-":
        outfile_exists = False  # Nothing will be overridden when writing to stdout.
    else:
        outfile_exists = os.path.exists(options.outfile)

    check_error(report_files_count, outfile_exists)

    if not outfile_exists:
        with utils.open_or_use_stdout(options.outfile) as fh:
            json.dump(combined_report, fh)

    if options.report_exit:
        sys.exit(report_exit(combined_test_report))
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
