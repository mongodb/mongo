#!/usr/bin/env python
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2008  Robert Collins <robertc@robertcollins.net>
#
#  Licensed under either the Apache License, Version 2.0 or the BSD 3-clause
#  license at the users choice. A copy of both licenses are available in the
#  project source as Apache-2.0 and BSD. You may not use this file except in
#  compliance with one of these two licences.
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under these licenses is distributed on an "AS IS" BASIS, WITHOUT
#  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
#  license you chose for the specific language governing permissions and
#  limitations under that license.
#

"""List tests in a subunit stream."""

import sys
from optparse import OptionParser

from testtools import CopyStreamResult, StreamResultRouter, StreamSummary

from subunit import ByteStreamToStreamResult
from subunit.filters import find_stream
from subunit.test_results import CatFiles, TestIdPrintingResult


def main():
    parser = OptionParser(description=__doc__)
    parser.add_option("--times", action="store_true",
        help="list the time each test took (requires a timestamped stream)",
            default=False)
    parser.add_option("--exists", action="store_true",
        help="list tests that are reported as existing (as well as ran)",
            default=False)
    parser.add_option("--no-passthrough", action="store_true",
        help="Hide all non subunit input.", default=False, dest="no_passthrough")
    (options, args) = parser.parse_args()
    test = ByteStreamToStreamResult(
        find_stream(sys.stdin, args), non_subunit_name="stdout")
    result = TestIdPrintingResult(sys.stdout, options.times, options.exists)
    if not options.no_passthrough:
        result = StreamResultRouter(result)
        cat = CatFiles(sys.stdout)
        result.add_rule(cat, 'test_id', test_id=None)
    summary = StreamSummary()
    result = CopyStreamResult([result, summary])
    result.startTestRun()
    test.run(result)
    result.stopTestRun()
    if summary.wasSuccessful():
        exit_code = 0
    else:
        exit_code = 1
    sys.exit(exit_code)


if __name__ == '__main__':
    main()
