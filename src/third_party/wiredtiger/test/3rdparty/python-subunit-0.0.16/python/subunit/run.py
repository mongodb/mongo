#!/usr/bin/python
#
# Simple subunit testrunner for python
# Copyright (C) Jelmer Vernooij <jelmer@samba.org> 2007
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

"""Run a unittest testcase reporting results as Subunit.

  $ python -m subunit.run mylib.tests.test_suite
"""

import io
import os
import sys

from testtools import ExtendedToStreamDecorator
from testtools.testsuite import iterate_tests

from subunit import StreamResultToBytes, get_default_formatter
from subunit.test_results import AutoTimingTestResultDecorator
from testtools.run import (
    BUFFEROUTPUT,
    CATCHBREAK,
    FAILFAST,
    list_test,
    TestProgram,
    USAGE_AS_MAIN,
    )


class SubunitTestRunner(object):
    def __init__(self, verbosity=None, failfast=None, buffer=None, stream=None):
        """Create a TestToolsTestRunner.

        :param verbosity: Ignored.
        :param failfast: Stop running tests at the first failure.
        :param buffer: Ignored.
        """
        self.failfast = failfast
        self.stream = stream or sys.stdout

    def run(self, test):
        "Run the given test case or test suite."
        result, _ = self._list(test)
        result = ExtendedToStreamDecorator(result)
        result = AutoTimingTestResultDecorator(result)
        if self.failfast is not None:
            result.failfast = self.failfast
        result.startTestRun()
        try:
            test(result)
        finally:
            result.stopTestRun()
        return result

    def list(self, test):
        "List the test."
        result, errors = self._list(test)
        if errors:
            failed_descr = '\n'.join(errors).encode('utf8')
            result.status(file_name="import errors", runnable=False,
                file_bytes=failed_descr, mime_type="text/plain;charset=utf8")
            sys.exit(2)

    def _list(self, test):
        test_ids, errors = list_test(test)
        try:
            fileno = self.stream.fileno()
        except:
            fileno = None
        if fileno is not None:
            stream = os.fdopen(fileno, 'wb', 0)
        else:
            stream = self.stream
        result = StreamResultToBytes(stream)
        for test_id in test_ids:
            result.status(test_id=test_id, test_status='exists')
        return result, errors


class SubunitTestProgram(TestProgram):

    USAGE = USAGE_AS_MAIN

    def usageExit(self, msg=None):
        if msg:
            print (msg)
        usage = {'progName': self.progName, 'catchbreak': '', 'failfast': '',
                 'buffer': ''}
        if self.failfast != False:
            usage['failfast'] = FAILFAST
        if self.catchbreak != False:
            usage['catchbreak'] = CATCHBREAK
        if self.buffer != False:
            usage['buffer'] = BUFFEROUTPUT
        usage_text = self.USAGE % usage
        usage_lines = usage_text.split('\n')
        usage_lines.insert(2, "Run a test suite with a subunit reporter.")
        usage_lines.insert(3, "")
        print('\n'.join(usage_lines))
        sys.exit(2)


def main():
    # Disable the default buffering, for Python 2.x where pdb doesn't do it
    # on non-ttys.
    stream = get_default_formatter()
    runner = SubunitTestRunner
    # Patch stdout to be unbuffered, so that pdb works well on 2.6/2.7.
    binstdout = io.open(sys.stdout.fileno(), 'wb', 0)
    if sys.version_info[0] > 2:
        sys.stdout = io.TextIOWrapper(binstdout, encoding=sys.stdout.encoding)
    else:
        sys.stdout = binstdout
    SubunitTestProgram(module=None, argv=sys.argv, testRunner=runner,
        stdout=sys.stdout)


if __name__ == '__main__':
    main()
