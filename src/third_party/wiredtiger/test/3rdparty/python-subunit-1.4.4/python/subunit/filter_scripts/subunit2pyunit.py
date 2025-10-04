#!/usr/bin/env python
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2009  Robert Collins <robertc@robertcollins.net>
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

"""Display a subunit stream through python's unittest test runner."""

import sys
import unittest
from operator import methodcaller
from optparse import OptionParser

from testtools import (DecorateTestCaseResult, StreamResultRouter,
                       StreamToExtendedDecorator)

from subunit import ByteStreamToStreamResult
from subunit.filters import find_stream
from subunit.test_results import CatFiles


def main():
    parser = OptionParser(description=__doc__)
    parser.add_option("--no-passthrough", action="store_true",
                      help="Hide all non subunit input.",
                      default=False, dest="no_passthrough")
    parser.add_option("--progress", action="store_true",
                      help="Use bzrlib's test reporter (requires bzrlib)",
                      default=False)
    (options, args) = parser.parse_args()
    test = ByteStreamToStreamResult(
        find_stream(sys.stdin, args), non_subunit_name='stdout')

    def wrap_result(result):
        result = StreamToExtendedDecorator(result)
        if not options.no_passthrough:
            result = StreamResultRouter(result)
            result.add_rule(CatFiles(sys.stdout), 'test_id', test_id=None)
        return result
    test = DecorateTestCaseResult(test, wrap_result,
                                  before_run=methodcaller('startTestRun'),
                                  after_run=methodcaller('stopTestRun'))
    if options.progress:
        from bzrlib import ui
        from bzrlib.tests import TextTestRunner
        ui.ui_factory = ui.make_ui_for_terminal(None, sys.stdout, sys.stderr)
        runner = TextTestRunner()
    else:
        runner = unittest.TextTestRunner(verbosity=2)
    if runner.run(test).wasSuccessful():
        exit_code = 0
    else:
        exit_code = 1
    sys.exit(exit_code)


if __name__ == '__main__':
    main()
