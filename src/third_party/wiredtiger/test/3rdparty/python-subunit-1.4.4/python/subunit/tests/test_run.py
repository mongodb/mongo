#
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2011  Robert Collins <robertc@robertcollins.net>
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

import io
import unittest

from testtools import PlaceHolder, TestCase
from testtools.compat import _b
from testtools.matchers import StartsWith
from testtools.testresult.doubles import StreamResult

import subunit
from subunit import run
from subunit.run import SubunitTestRunner


class TestSubunitTestRunner(TestCase):

    def test_includes_timing_output(self):
        bytestream = io.BytesIO()
        runner = SubunitTestRunner(stream=bytestream)
        test = PlaceHolder('name')
        runner.run(test)
        bytestream.seek(0)
        eventstream = StreamResult()
        subunit.ByteStreamToStreamResult(bytestream).run(eventstream)
        timestamps = [event[-1] for event in eventstream._events
            if event is not None]
        self.assertNotEqual([], timestamps)

    def test_enumerates_tests_before_run(self):
        bytestream = io.BytesIO()
        runner = SubunitTestRunner(stream=bytestream)
        test1 = PlaceHolder('name1')
        test2 = PlaceHolder('name2')
        case = unittest.TestSuite([test1, test2])
        runner.run(case)
        bytestream.seek(0)
        eventstream = StreamResult()
        subunit.ByteStreamToStreamResult(bytestream).run(eventstream)
        self.assertEqual([
            ('status', 'name1', 'exists'),
            ('status', 'name2', 'exists'),
            ], [event[:3] for event in eventstream._events[:2]])

    def test_list_errors_if_errors_from_list_test(self):
        bytestream = io.BytesIO()
        runner = SubunitTestRunner(stream=bytestream)
        def list_test(test):
            return [], ['failed import']
        self.patch(run, 'list_test', list_test)
        exc = self.assertRaises(SystemExit, runner.list, None)
        self.assertEqual((2,), exc.args)

    def test_list_includes_loader_errors(self):
        bytestream = io.BytesIO()
        runner = SubunitTestRunner(stream=bytestream)
        def list_test(test):
            return [], []
        class Loader:
            errors = ['failed import']
        loader = Loader()
        self.patch(run, 'list_test', list_test)
        exc = self.assertRaises(SystemExit, runner.list, None, loader=loader)
        self.assertEqual((2,), exc.args)

    class FailingTest(TestCase):
        def test_fail(self):
            1/0

    def test_exits_zero_when_tests_fail(self):
        bytestream = io.BytesIO()
        stream = io.TextIOWrapper(bytestream, encoding="utf8")
        try:
            self.assertEqual(None, run.main(
                argv=["progName", "subunit.tests.test_run.TestSubunitTestRunner.FailingTest"],
                stdout=stream))
        except SystemExit:
            self.fail("SystemExit raised")
        self.assertThat(bytestream.getvalue(), StartsWith(_b('\xb3')))

    class ExitingTest(TestCase):
        def test_exit(self):
            raise SystemExit(0)

    def test_exits_nonzero_when_execution_errors(self):
        bytestream = io.BytesIO()
        stream = io.TextIOWrapper(bytestream, encoding="utf8")
        exc = self.assertRaises(SystemExit, run.main,
                argv=["progName", "subunit.tests.test_run.TestSubunitTestRunner.ExitingTest"],
                stdout=stream)
        self.assertEqual(0, exc.args[0])
