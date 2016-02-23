#
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2005  Robert Collins <robertc@robertcollins.net>
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

"""Tests for subunit.TestResultFilter."""

from datetime import datetime
import os
import subprocess
import sys
from subunit import iso8601
import unittest

from testtools import TestCase
from testtools.compat import _b, BytesIO
from testtools.testresult.doubles import ExtendedTestResult, StreamResult

import subunit
from subunit.test_results import make_tag_filter, TestResultFilter
from subunit import ByteStreamToStreamResult, StreamResultToBytes


class TestTestResultFilter(TestCase):
    """Test for TestResultFilter, a TestResult object which filters tests."""

    # While TestResultFilter works on python objects, using a subunit stream
    # is an easy pithy way of getting a series of test objects to call into
    # the TestResult, and as TestResultFilter is intended for use with subunit
    # also has the benefit of detecting any interface skew issues.
    example_subunit_stream = _b("""\
tags: global
test passed
success passed
test failed
tags: local
failure failed
test error
error error [
error details
]
test skipped
skip skipped
test todo
xfail todo
""")

    def run_tests(self, result_filter, input_stream=None):
        """Run tests through the given filter.

        :param result_filter: A filtering TestResult object.
        :param input_stream: Bytes of subunit stream data. If not provided,
            uses TestTestResultFilter.example_subunit_stream.
        """
        if input_stream is None:
            input_stream = self.example_subunit_stream
        test = subunit.ProtocolTestCase(BytesIO(input_stream))
        test.run(result_filter)

    def test_default(self):
        """The default is to exclude success and include everything else."""
        filtered_result = unittest.TestResult()
        result_filter = TestResultFilter(filtered_result)
        self.run_tests(result_filter)
        # skips are seen as success by default python TestResult.
        self.assertEqual(['error'],
            [error[0].id() for error in filtered_result.errors])
        self.assertEqual(['failed'],
            [failure[0].id() for failure in
            filtered_result.failures])
        self.assertEqual(4, filtered_result.testsRun)

    def test_tag_filter(self):
        tag_filter = make_tag_filter(['global'], ['local'])
        result = ExtendedTestResult()
        result_filter = TestResultFilter(
            result, filter_success=False, filter_predicate=tag_filter)
        self.run_tests(result_filter)
        tests_included = [
            event[1] for event in result._events if event[0] == 'startTest']
        tests_expected = list(map(
            subunit.RemotedTestCase,
            ['passed', 'error', 'skipped', 'todo']))
        self.assertEquals(tests_expected, tests_included)

    def test_tags_tracked_correctly(self):
        tag_filter = make_tag_filter(['a'], [])
        result = ExtendedTestResult()
        result_filter = TestResultFilter(
            result, filter_success=False, filter_predicate=tag_filter)
        input_stream = _b(
            "test: foo\n"
            "tags: a\n"
            "successful: foo\n"
            "test: bar\n"
            "successful: bar\n")
        self.run_tests(result_filter, input_stream)
        foo = subunit.RemotedTestCase('foo')
        self.assertEquals(
            [('startTest', foo),
             ('tags', set(['a']), set()),
             ('addSuccess', foo),
             ('stopTest', foo),
             ],
            result._events)

    def test_exclude_errors(self):
        filtered_result = unittest.TestResult()
        result_filter = TestResultFilter(filtered_result, filter_error=True)
        self.run_tests(result_filter)
        # skips are seen as errors by default python TestResult.
        self.assertEqual([], filtered_result.errors)
        self.assertEqual(['failed'],
            [failure[0].id() for failure in
            filtered_result.failures])
        self.assertEqual(3, filtered_result.testsRun)

    def test_fixup_expected_failures(self):
        filtered_result = unittest.TestResult()
        result_filter = TestResultFilter(filtered_result,
            fixup_expected_failures=set(["failed"]))
        self.run_tests(result_filter)
        self.assertEqual(['failed', 'todo'],
            [failure[0].id() for failure in filtered_result.expectedFailures])
        self.assertEqual([], filtered_result.failures)
        self.assertEqual(4, filtered_result.testsRun)

    def test_fixup_expected_errors(self):
        filtered_result = unittest.TestResult()
        result_filter = TestResultFilter(filtered_result,
            fixup_expected_failures=set(["error"]))
        self.run_tests(result_filter)
        self.assertEqual(['error', 'todo'],
            [failure[0].id() for failure in filtered_result.expectedFailures])
        self.assertEqual([], filtered_result.errors)
        self.assertEqual(4, filtered_result.testsRun)

    def test_fixup_unexpected_success(self):
        filtered_result = unittest.TestResult()
        result_filter = TestResultFilter(filtered_result, filter_success=False,
            fixup_expected_failures=set(["passed"]))
        self.run_tests(result_filter)
        self.assertEqual(['passed'],
            [passed.id() for passed in filtered_result.unexpectedSuccesses])
        self.assertEqual(5, filtered_result.testsRun)

    def test_exclude_failure(self):
        filtered_result = unittest.TestResult()
        result_filter = TestResultFilter(filtered_result, filter_failure=True)
        self.run_tests(result_filter)
        self.assertEqual(['error'],
            [error[0].id() for error in filtered_result.errors])
        self.assertEqual([],
            [failure[0].id() for failure in
            filtered_result.failures])
        self.assertEqual(3, filtered_result.testsRun)

    def test_exclude_skips(self):
        filtered_result = subunit.TestResultStats(None)
        result_filter = TestResultFilter(filtered_result, filter_skip=True)
        self.run_tests(result_filter)
        self.assertEqual(0, filtered_result.skipped_tests)
        self.assertEqual(2, filtered_result.failed_tests)
        self.assertEqual(3, filtered_result.testsRun)

    def test_include_success(self):
        """Successes can be included if requested."""
        filtered_result = unittest.TestResult()
        result_filter = TestResultFilter(filtered_result,
            filter_success=False)
        self.run_tests(result_filter)
        self.assertEqual(['error'],
            [error[0].id() for error in filtered_result.errors])
        self.assertEqual(['failed'],
            [failure[0].id() for failure in
            filtered_result.failures])
        self.assertEqual(5, filtered_result.testsRun)

    def test_filter_predicate(self):
        """You can filter by predicate callbacks"""
        # 0.0.7 and earlier did not support the 'tags' parameter, so we need
        # to test that we still support behaviour without it.
        filtered_result = unittest.TestResult()
        def filter_cb(test, outcome, err, details):
            return outcome == 'success'
        result_filter = TestResultFilter(filtered_result,
            filter_predicate=filter_cb,
            filter_success=False)
        self.run_tests(result_filter)
        # Only success should pass
        self.assertEqual(1, filtered_result.testsRun)

    def test_filter_predicate_with_tags(self):
        """You can filter by predicate callbacks that accept tags"""
        filtered_result = unittest.TestResult()
        def filter_cb(test, outcome, err, details, tags):
            return outcome == 'success'
        result_filter = TestResultFilter(filtered_result,
            filter_predicate=filter_cb,
            filter_success=False)
        self.run_tests(result_filter)
        # Only success should pass
        self.assertEqual(1, filtered_result.testsRun)

    def test_time_ordering_preserved(self):
        # Passing a subunit stream through TestResultFilter preserves the
        # relative ordering of 'time' directives and any other subunit
        # directives that are still included.
        date_a = datetime(year=2000, month=1, day=1, tzinfo=iso8601.UTC)
        date_b = datetime(year=2000, month=1, day=2, tzinfo=iso8601.UTC)
        date_c = datetime(year=2000, month=1, day=3, tzinfo=iso8601.UTC)
        subunit_stream = _b('\n'.join([
            "time: %s",
            "test: foo",
            "time: %s",
            "error: foo",
            "time: %s",
            ""]) % (date_a, date_b, date_c))
        result = ExtendedTestResult()
        result_filter = TestResultFilter(result)
        self.run_tests(result_filter, subunit_stream)
        foo = subunit.RemotedTestCase('foo')
        self.maxDiff = None
        self.assertEqual(
            [('time', date_a),
             ('time', date_b),
             ('startTest', foo),
             ('addError', foo, {}),
             ('stopTest', foo),
             ('time', date_c)], result._events)

    def test_time_passes_through_filtered_tests(self):
        # Passing a subunit stream through TestResultFilter preserves 'time'
        # directives even if a specific test is filtered out.
        date_a = datetime(year=2000, month=1, day=1, tzinfo=iso8601.UTC)
        date_b = datetime(year=2000, month=1, day=2, tzinfo=iso8601.UTC)
        date_c = datetime(year=2000, month=1, day=3, tzinfo=iso8601.UTC)
        subunit_stream = _b('\n'.join([
            "time: %s",
            "test: foo",
            "time: %s",
            "success: foo",
            "time: %s",
            ""]) % (date_a, date_b, date_c))
        result = ExtendedTestResult()
        result_filter = TestResultFilter(result)
        result_filter.startTestRun()
        self.run_tests(result_filter, subunit_stream)
        result_filter.stopTestRun()
        foo = subunit.RemotedTestCase('foo')
        self.maxDiff = None
        self.assertEqual(
            [('startTestRun',),
             ('time', date_a),
             ('time', date_c),
             ('stopTestRun',),], result._events)

    def test_skip_preserved(self):
        subunit_stream = _b('\n'.join([
            "test: foo",
            "skip: foo",
            ""]))
        result = ExtendedTestResult()
        result_filter = TestResultFilter(result)
        self.run_tests(result_filter, subunit_stream)
        foo = subunit.RemotedTestCase('foo')
        self.assertEquals(
            [('startTest', foo),
             ('addSkip', foo, {}),
             ('stopTest', foo), ], result._events)

    if sys.version_info < (2, 7):
        # These tests require Python >=2.7.
        del test_fixup_expected_failures, test_fixup_expected_errors, test_fixup_unexpected_success


class TestFilterCommand(TestCase):

    def run_command(self, args, stream):
        root = os.path.dirname(
            os.path.dirname(os.path.dirname(os.path.dirname(__file__))))
        script_path = os.path.join(root, 'filters', 'subunit-filter')
        command = [sys.executable, script_path] + list(args)
        ps = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, err = ps.communicate(stream)
        if ps.returncode != 0:
            raise RuntimeError("%s failed: %s" % (command, err))
        return out

    def test_default(self):
        byte_stream = BytesIO()
        stream = StreamResultToBytes(byte_stream)
        stream.status(test_id="foo", test_status="inprogress")
        stream.status(test_id="foo", test_status="skip")
        output = self.run_command([], byte_stream.getvalue())
        events = StreamResult()
        ByteStreamToStreamResult(BytesIO(output)).run(events)
        ids = set(event[1] for event in events._events)
        self.assertEqual([
            ('status', 'foo', 'inprogress'),
            ('status', 'foo', 'skip'),
            ], [event[:3] for event in events._events])

    def test_tags(self):
        byte_stream = BytesIO()
        stream = StreamResultToBytes(byte_stream)
        stream.status(
            test_id="foo", test_status="inprogress", test_tags=set(["a"]))
        stream.status(
            test_id="foo", test_status="success", test_tags=set(["a"]))
        stream.status(test_id="bar", test_status="inprogress")
        stream.status(test_id="bar", test_status="inprogress")
        stream.status(
            test_id="baz", test_status="inprogress", test_tags=set(["a"]))
        stream.status(
            test_id="baz", test_status="success", test_tags=set(["a"]))
        output = self.run_command(
            ['-s', '--with-tag', 'a'], byte_stream.getvalue())
        events = StreamResult()
        ByteStreamToStreamResult(BytesIO(output)).run(events)
        ids = set(event[1] for event in events._events)
        self.assertEqual(set(['foo', 'baz']), ids)

    def test_no_passthrough(self):
        output = self.run_command(['--no-passthrough'], b'hi thar')
        self.assertEqual(b'', output)

    def test_passthrough(self):
        output = self.run_command([], b'hi thar')
        byte_stream = BytesIO()
        stream = StreamResultToBytes(byte_stream)
        stream.status(file_name="stdout", file_bytes=b'hi thar')
        self.assertEqual(byte_stream.getvalue(), output)
