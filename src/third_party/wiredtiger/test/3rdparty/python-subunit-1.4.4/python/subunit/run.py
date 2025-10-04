#!/usr/bin/python3
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
from testtools.run import (BUFFEROUTPUT, CATCHBREAK, FAILFAST, USAGE_AS_MAIN,
                           TestProgram, list_test)

from subunit import StreamResultToBytes
from subunit.test_results import AutoTimingTestResultDecorator


class SubunitTestRunner(object):
    def __init__(self, verbosity=None, failfast=None, buffer=None, stream=None,
        stdout=None, tb_locals=False):
        """Create a TestToolsTestRunner.

        :param verbosity: Ignored.
        :param failfast: Stop running tests at the first failure.
        :param buffer: Ignored.
        :param stream: Upstream unittest stream parameter.
        :param stdout: Testtools stream parameter.
        :param tb_locals: Testtools traceback in locals parameter.

        Either stream or stdout can be supplied, and stream will take
        precedence.
        """
        self.failfast = failfast
        self.stream = stream or stdout or sys.stdout
        self.tb_locals = tb_locals

    def run(self, test):
        "Run the given test case or test suite."
        result, _ = self._list(test)
        result = ExtendedToStreamDecorator(result)
        result = AutoTimingTestResultDecorator(result)
        if self.failfast is not None:
            result.failfast = self.failfast
            result.tb_locals = self.tb_locals
        result.startTestRun()
        try:
            test(result)
        finally:
            result.stopTestRun()
        return result

    def list(self, test, loader=None):
        "List the test."
        result, errors = self._list(test)
        if loader is not None:
            # We were called with the updated API by testtools.run, so look for
            # errors on the loader, not the test list result.
            errors = loader.errors
        if errors:
            failed_descr = '\n'.join(errors).encode('utf8')
            result.status(file_name="import errors", runnable=False,
                file_bytes=failed_descr, mime_type="text/plain;charset=utf8")
            sys.exit(2)

    def _list(self, test):
        test_ids, errors = list_test(test)
        try:
            fileno = self.stream.fileno()
        except:  # noqa: E722
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
        if self.failfast is not False:
            usage['failfast'] = FAILFAST
        if self.catchbreak is not False:
            usage['catchbreak'] = CATCHBREAK
        if self.buffer is not False:
            usage['buffer'] = BUFFEROUTPUT
        usage_text = self.USAGE % usage
        usage_lines = usage_text.split('\n')
        usage_lines.insert(2, "Run a test suite with a subunit reporter.")
        usage_lines.insert(3, "")
        print('\n'.join(usage_lines))
        sys.exit(2)


def main(argv=None, stdout=None):
    if argv is None:
        argv = sys.argv
    runner = SubunitTestRunner
    # stdout is None except in unit tests.
    if stdout is None:
        stdout = sys.stdout
        # Disable the default buffering, for Python 2.x where pdb doesn't do it
        # on non-ttys.
        if hasattr(stdout, 'fileno'):
            # Patch stdout to be unbuffered, so that pdb works well on 2.6/2.7.
            binstdout = io.open(stdout.fileno(), 'wb', 0)
            sys.stdout = io.TextIOWrapper(binstdout, encoding=sys.stdout.encoding)
            stdout = sys.stdout
    SubunitTestProgram(module=None, argv=argv, testRunner=runner,
        stdout=stdout, exit=False)


if __name__ == '__main__':
    main()
