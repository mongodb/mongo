#!/usr/bin/env python
"""
Converts silent test failures into non-silent failures.

Any test files with at least 2 executions in the report.json file that have a "silentfail" status,
this script will change the outputted report to have a "fail" status instead.
"""

from __future__ import absolute_import
from __future__ import print_function

import collections
import json
import optparse
import os
import sys


# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from buildscripts.resmokelib.testing import report


def read_json_file(json_file):
    with open(json_file) as json_data:
        return json.load(json_data)


def main():

    usage = "usage: %prog [options] report.json"
    parser = optparse.OptionParser(usage=usage)
    parser.add_option("-o", "--output-file",
                      dest="outfile",
                      default="-",
                      help="If '-', then the report file is written to stdout."
                           " Any other value is treated as the output file name. By default,"
                           " output is written to stdout.")

    (options, args) = parser.parse_args()

    if len(args) != 1:
        parser.error("Requires a single report.json file.")

    report_file_json = read_json_file(args[0])
    test_report = report.TestReport.from_dict(report_file_json)

    # Count number of "silentfail" per test file.
    status_dict = collections.defaultdict(int)
    for test_info in test_report.test_infos:
        if test_info.status == "silentfail":
            status_dict[test_info.test_id] += 1

    # For test files with more than 1 "silentfail", convert status to "fail".
    for test_info in test_report.test_infos:
        if status_dict[test_info.test_id] >= 2:
            test_info.status = "fail"

    result_report = test_report.as_dict();
    if options.outfile != "-":
        with open(options.outfile, "w") as fp:
            json.dump(result_report, fp)
    else:
        print(json.dumps(result_report))

if __name__ == "__main__":
    main()
