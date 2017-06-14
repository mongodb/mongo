#!/usr/bin/env python

"""
Combines JSON report files used in Evergreen
"""

from __future__ import absolute_import
from __future__ import print_function

import errno
import json
import os
import sys
from optparse import OptionParser

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from buildscripts.resmokelib.testing import report


def read_json_file(json_file):
    with open(json_file) as json_data:
        return json.load(json_data)


def report_exit(combined_test_report):
    """The exit code of this script is based on the following:
        0:  All tests have status "pass", or only non-dynamic tests have status "silentfail".
        31: At least one test has status "fail" or "timeout".
       Note: A test can be considered dynamic if its name contains a ":" character."""

    ret = 0
    for test in combined_test_report.test_infos:
        if test.status in ["fail", "timeout"]:
            return 31
    return ret


def main():

    usage = "usage: %prog [options] report1.json report2.json ..."
    parser = OptionParser(description=__doc__, usage=usage)
    parser.add_option("-o", "--output-file",
                      dest="outfile",
                      default="-",
                      help="If '-', then the combined report file is written to stdout."
                           " Any other value is treated as the output file name. By default,"
                           " output is written to stdout.")
    parser.add_option("-m", "--ignore-missing-reports",
                      dest="ignore_missing",
                      action="store_true",
                      help="Ignore any input report file that does not exist.")

    (options, args) = parser.parse_args()

    if not args:
        sys.exit("No report files were specified")

    report_files = args
    test_reports = []
    for report_file in report_files:
        try:
            report_file_json = read_json_file(report_file)
            test_reports.append(report.TestReport.from_dict(report_file_json))
        except IOError as e:
            # errno.ENOENT is the error code for "No such file or directory".
            if options.ignore_missing and e.errno == errno.ENOENT:
                print("Ignoring missing file {}".format(report_file))
                continue
            raise

    combined_test_report = report.TestReport.combine(*test_reports)
    combined_report = combined_test_report.as_dict()
    if options.outfile != "-":
        with open(options.outfile, "w") as fstream:
            json.dump(combined_report, fstream)
    else:
        print(json.dumps(combined_report))

    sys.exit(report_exit(combined_test_report))


if __name__ == "__main__":
    main()
