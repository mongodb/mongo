#!/usr/bin/env python3
"""Combine JSON report files used in Evergreen."""

import errno
import json
import os
import sys
from optparse import OptionParser

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.resmokelib.testing import report  # pylint: disable=wrong-import-position
from buildscripts.resmokelib import utils  # pylint: disable=wrong-import-position


def read_json_file(json_file):
    """Read JSON file."""
    with open(json_file) as json_data:
        return json.load(json_data)


def report_exit(combined_test_report):
    """Return report exit code.

    The exit code of this script is based on the following:
        0:  All tests have status "pass", or only non-dynamic tests have status "silentfail".
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


def main():
    """Execute Main program."""
    usage = "usage: %prog [options] report1.json report2.json ..."
    parser = OptionParser(description=__doc__, usage=usage)
    parser.add_option(
        "-o", "--output-file", dest="outfile", default="-",
        help=("If '-', then the combined report file is written to stdout."
              " Any other value is treated as the output file name. By default,"
              " output is written to stdout."))
    parser.add_option("-x", "--no-report-exit", dest="report_exit", default=True,
                      action="store_false",
                      help="Do not exit with a non-zero code if any test in the report fails.")

    (options, args) = parser.parse_args()

    if not args:
        sys.exit("No report files were specified")

    report_files = args
    report_files_count = len(report_files)
    test_reports = []

    for report_file in report_files:
        try:
            report_file_json = read_json_file(report_file)
            test_reports.append(report.TestReport.from_dict(report_file_json))
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
