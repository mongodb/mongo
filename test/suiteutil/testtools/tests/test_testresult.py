# Copyright (c) 2008 testtools developers. See LICENSE for details.

"""Test TestResults and related things."""

__metaclass__ = type

import codecs
import datetime
import doctest
import os
import shutil
import sys
import tempfile
import threading
import warnings

from testtools import (
    ExtendedToOriginalDecorator,
    MultiTestResult,
    TestCase,
    TestResult,
    TextTestResult,
    ThreadsafeForwardingResult,
    testresult,
    )
from testtools.compat import (
    _b,
    _get_exception_encoding,
    _r,
    _u,
    str_is_unicode,
    StringIO,
    )
from testtools.content import Content
from testtools.content_type import ContentType, UTF8_TEXT
from testtools.matchers import (
    DocTestMatches,
    MatchesException,
    Raises,
    )
from testtools.tests.helpers import (
    LoggingResult,
    Python26TestResult,
    Python27TestResult,
    ExtendedTestResult,
    an_exc_info
    )
from testtools.testresult.real import utc


class Python26Contract(object):

    def test_fresh_result_is_successful(self):
        # A result is considered successful before any tests are run.
        result = self.makeResult()
        self.assertTrue(result.wasSuccessful())

    def test_addError_is_failure(self):
        # addError fails the test run.
        result = self.makeResult()
        result.startTest(self)
        result.addError(self, an_exc_info)
        result.stopTest(self)
        self.assertFalse(result.wasSuccessful())

    def test_addFailure_is_failure(self):
        # addFailure fails the test run.
        result = self.makeResult()
        result.startTest(self)
        result.addFailure(self, an_exc_info)
        result.stopTest(self)
        self.assertFalse(result.wasSuccessful())

    def test_addSuccess_is_success(self):
        # addSuccess does not fail the test run.
        result = self.makeResult()
        result.startTest(self)
        result.addSuccess(self)
        result.stopTest(self)
        self.assertTrue(result.wasSuccessful())


class Python27Contract(Python26Contract):

    def test_addExpectedFailure(self):
        # Calling addExpectedFailure(test, exc_info) completes ok.
        result = self.makeResult()
        result.startTest(self)
        result.addExpectedFailure(self, an_exc_info)

    def test_addExpectedFailure_is_success(self):
        # addExpectedFailure does not fail the test run.
        result = self.makeResult()
        result.startTest(self)
        result.addExpectedFailure(self, an_exc_info)
        result.stopTest(self)
        self.assertTrue(result.wasSuccessful())

    def test_addSkipped(self):
        # Calling addSkip(test, reason) completes ok.
        result = self.makeResult()
        result.startTest(self)
        result.addSkip(self, _u("Skipped for some reason"))

    def test_addSkip_is_success(self):
        # addSkip does not fail the test run.
        result = self.makeResult()
        result.startTest(self)
        result.addSkip(self, _u("Skipped for some reason"))
        result.stopTest(self)
        self.assertTrue(result.wasSuccessful())

    def test_addUnexpectedSuccess(self):
        # Calling addUnexpectedSuccess(test) completes ok.
        result = self.makeResult()
        result.startTest(self)
        result.addUnexpectedSuccess(self)

    def test_addUnexpectedSuccess_was_successful(self):
        # addUnexpectedSuccess does not fail the test run in Python 2.7.
        result = self.makeResult()
        result.startTest(self)
        result.addUnexpectedSuccess(self)
        result.stopTest(self)
        self.assertTrue(result.wasSuccessful())

    def test_startStopTestRun(self):
        # Calling startTestRun completes ok.
        result = self.makeResult()
        result.startTestRun()
        result.stopTestRun()


class DetailsContract(Python27Contract):
    """Tests for the contract of TestResults."""

    def test_addExpectedFailure_details(self):
        # Calling addExpectedFailure(test, details=xxx) completes ok.
        result = self.makeResult()
        result.startTest(self)
        result.addExpectedFailure(self, details={})

    def test_addError_details(self):
        # Calling addError(test, details=xxx) completes ok.
        result = self.makeResult()
        result.startTest(self)
        result.addError(self, details={})

    def test_addFailure_details(self):
        # Calling addFailure(test, details=xxx) completes ok.
        result = self.makeResult()
        result.startTest(self)
        result.addFailure(self, details={})

    def test_addSkipped_details(self):
        # Calling addSkip(test, reason) completes ok.
        result = self.makeResult()
        result.startTest(self)
        result.addSkip(self, details={})

    def test_addUnexpectedSuccess_details(self):
        # Calling addUnexpectedSuccess(test) completes ok.
        result = self.makeResult()
        result.startTest(self)
        result.addUnexpectedSuccess(self, details={})

    def test_addSuccess_details(self):
        # Calling addSuccess(test) completes ok.
        result = self.makeResult()
        result.startTest(self)
        result.addSuccess(self, details={})


class FallbackContract(DetailsContract):
    """When we fallback we take our policy choice to map calls.

    For instance, we map unexpectedSuccess to an error code, not to success.
    """

    def test_addUnexpectedSuccess_was_successful(self):
        # addUnexpectedSuccess fails test run in testtools.
        result = self.makeResult()
        result.startTest(self)
        result.addUnexpectedSuccess(self)
        result.stopTest(self)
        self.assertFalse(result.wasSuccessful())


class StartTestRunContract(FallbackContract):
    """Defines the contract for testtools policy choices.
    
    That is things which are not simply extensions to unittest but choices we
    have made differently.
    """

    def test_startTestRun_resets_unexpected_success(self):
        result = self.makeResult()
        result.startTest(self)
        result.addUnexpectedSuccess(self)
        result.stopTest(self)
        result.startTestRun()
        self.assertTrue(result.wasSuccessful())

    def test_startTestRun_resets_failure(self):
        result = self.makeResult()
        result.startTest(self)
        result.addFailure(self, an_exc_info)
        result.stopTest(self)
        result.startTestRun()
        self.assertTrue(result.wasSuccessful())

    def test_startTestRun_resets_errors(self):
        result = self.makeResult()
        result.startTest(self)
        result.addError(self, an_exc_info)
        result.stopTest(self)
        result.startTestRun()
        self.assertTrue(result.wasSuccessful())


class TestTestResultContract(TestCase, StartTestRunContract):

    def makeResult(self):
        return TestResult()


class TestMultiTestResultContract(TestCase, StartTestRunContract):

    def makeResult(self):
        return MultiTestResult(TestResult(), TestResult())


class TestTextTestResultContract(TestCase, StartTestRunContract):

    def makeResult(self):
        return TextTestResult(StringIO())


class TestThreadSafeForwardingResultContract(TestCase, StartTestRunContract):

    def makeResult(self):
        result_semaphore = threading.Semaphore(1)
        target = TestResult()
        return ThreadsafeForwardingResult(target, result_semaphore)


class TestExtendedTestResultContract(TestCase, StartTestRunContract):

    def makeResult(self):
        return ExtendedTestResult()


class TestPython26TestResultContract(TestCase, Python26Contract):

    def makeResult(self):
        return Python26TestResult()


class TestAdaptedPython26TestResultContract(TestCase, FallbackContract):

    def makeResult(self):
        return ExtendedToOriginalDecorator(Python26TestResult())


class TestPython27TestResultContract(TestCase, Python27Contract):

    def makeResult(self):
        return Python27TestResult()


class TestAdaptedPython27TestResultContract(TestCase, DetailsContract):

    def makeResult(self):
        return ExtendedToOriginalDecorator(Python27TestResult())


class TestTestResult(TestCase):
    """Tests for 'TestResult'."""

    def makeResult(self):
        """Make an arbitrary result for testing."""
        return TestResult()

    def test_addSkipped(self):
        # Calling addSkip on a TestResult records the test that was skipped in
        # its skip_reasons dict.
        result = self.makeResult()
        result.addSkip(self, _u("Skipped for some reason"))
        self.assertEqual({_u("Skipped for some reason"):[self]},
            result.skip_reasons)
        result.addSkip(self, _u("Skipped for some reason"))
        self.assertEqual({_u("Skipped for some reason"):[self, self]},
            result.skip_reasons)
        result.addSkip(self, _u("Skipped for another reason"))
        self.assertEqual({_u("Skipped for some reason"):[self, self],
            _u("Skipped for another reason"):[self]},
            result.skip_reasons)

    def test_now_datetime_now(self):
        result = self.makeResult()
        olddatetime = testresult.real.datetime
        def restore():
            testresult.real.datetime = olddatetime
        self.addCleanup(restore)
        class Module:
            pass
        now = datetime.datetime.now(utc)
        stubdatetime = Module()
        stubdatetime.datetime = Module()
        stubdatetime.datetime.now = lambda tz: now
        testresult.real.datetime = stubdatetime
        # Calling _now() looks up the time.
        self.assertEqual(now, result._now())
        then = now + datetime.timedelta(0, 1)
        # Set an explicit datetime, which gets returned from then on.
        result.time(then)
        self.assertNotEqual(now, result._now())
        self.assertEqual(then, result._now())
        # go back to looking it up.
        result.time(None)
        self.assertEqual(now, result._now())

    def test_now_datetime_time(self):
        result = self.makeResult()
        now = datetime.datetime.now(utc)
        result.time(now)
        self.assertEqual(now, result._now())


class TestWithFakeExceptions(TestCase):

    def makeExceptionInfo(self, exceptionFactory, *args, **kwargs):
        try:
            raise exceptionFactory(*args, **kwargs)
        except:
            return sys.exc_info()


class TestMultiTestResult(TestWithFakeExceptions):
    """Tests for 'MultiTestResult'."""

    def setUp(self):
        TestWithFakeExceptions.setUp(self)
        self.result1 = LoggingResult([])
        self.result2 = LoggingResult([])
        self.multiResult = MultiTestResult(self.result1, self.result2)

    def assertResultLogsEqual(self, expectedEvents):
        """Assert that our test results have received the expected events."""
        self.assertEqual(expectedEvents, self.result1._events)
        self.assertEqual(expectedEvents, self.result2._events)

    def test_empty(self):
        # Initializing a `MultiTestResult` doesn't do anything to its
        # `TestResult`s.
        self.assertResultLogsEqual([])

    def test_startTest(self):
        # Calling `startTest` on a `MultiTestResult` calls `startTest` on all
        # its `TestResult`s.
        self.multiResult.startTest(self)
        self.assertResultLogsEqual([('startTest', self)])

    def test_stopTest(self):
        # Calling `stopTest` on a `MultiTestResult` calls `stopTest` on all
        # its `TestResult`s.
        self.multiResult.stopTest(self)
        self.assertResultLogsEqual([('stopTest', self)])

    def test_addSkipped(self):
        # Calling `addSkip` on a `MultiTestResult` calls addSkip on its
        # results.
        reason = _u("Skipped for some reason")
        self.multiResult.addSkip(self, reason)
        self.assertResultLogsEqual([('addSkip', self, reason)])

    def test_addSuccess(self):
        # Calling `addSuccess` on a `MultiTestResult` calls `addSuccess` on
        # all its `TestResult`s.
        self.multiResult.addSuccess(self)
        self.assertResultLogsEqual([('addSuccess', self)])

    def test_done(self):
        # Calling `done` on a `MultiTestResult` calls `done` on all its
        # `TestResult`s.
        self.multiResult.done()
        self.assertResultLogsEqual([('done')])

    def test_addFailure(self):
        # Calling `addFailure` on a `MultiTestResult` calls `addFailure` on
        # all its `TestResult`s.
        exc_info = self.makeExceptionInfo(AssertionError, 'failure')
        self.multiResult.addFailure(self, exc_info)
        self.assertResultLogsEqual([('addFailure', self, exc_info)])

    def test_addError(self):
        # Calling `addError` on a `MultiTestResult` calls `addError` on all
        # its `TestResult`s.
        exc_info = self.makeExceptionInfo(RuntimeError, 'error')
        self.multiResult.addError(self, exc_info)
        self.assertResultLogsEqual([('addError', self, exc_info)])

    def test_startTestRun(self):
        # Calling `startTestRun` on a `MultiTestResult` forwards to all its
        # `TestResult`s.
        self.multiResult.startTestRun()
        self.assertResultLogsEqual([('startTestRun')])

    def test_stopTestRun(self):
        # Calling `stopTestRun` on a `MultiTestResult` forwards to all its
        # `TestResult`s.
        self.multiResult.stopTestRun()
        self.assertResultLogsEqual([('stopTestRun')])

    def test_stopTestRun_returns_results(self):
        # `MultiTestResult.stopTestRun` returns a tuple of all of the return
        # values the `stopTestRun`s that it forwards to.
        class Result(LoggingResult):
            def stopTestRun(self):
                super(Result, self).stopTestRun()
                return 'foo'
        multi_result = MultiTestResult(Result([]), Result([]))
        result = multi_result.stopTestRun()
        self.assertEqual(('foo', 'foo'), result)

    def test_time(self):
        # the time call is dispatched, not eaten by the base class
        self.multiResult.time('foo')
        self.assertResultLogsEqual([('time', 'foo')])


class TestTextTestResult(TestCase):
    """Tests for 'TextTestResult'."""

    def setUp(self):
        super(TestTextTestResult, self).setUp()
        self.result = TextTestResult(StringIO())

    def make_erroring_test(self):
        class Test(TestCase):
            def error(self):
                1/0
        return Test("error")

    def make_failing_test(self):
        class Test(TestCase):
            def failed(self):
                self.fail("yo!")
        return Test("failed")

    def make_unexpectedly_successful_test(self):
        class Test(TestCase):
            def succeeded(self):
                self.expectFailure("yo!", lambda: None)
        return Test("succeeded")

    def make_test(self):
        class Test(TestCase):
            def test(self):
                pass
        return Test("test")

    def getvalue(self):
        return self.result.stream.getvalue()

    def test__init_sets_stream(self):
        result = TextTestResult("fp")
        self.assertEqual("fp", result.stream)

    def reset_output(self):
        self.result.stream = StringIO()

    def test_startTestRun(self):
        self.result.startTestRun()
        self.assertEqual("Tests running...\n", self.getvalue())

    def test_stopTestRun_count_many(self):
        test = self.make_test()
        self.result.startTestRun()
        self.result.startTest(test)
        self.result.stopTest(test)
        self.result.startTest(test)
        self.result.stopTest(test)
        self.result.stream = StringIO()
        self.result.stopTestRun()
        self.assertThat(self.getvalue(),
            DocTestMatches("Ran 2 tests in ...s\n...", doctest.ELLIPSIS))

    def test_stopTestRun_count_single(self):
        test = self.make_test()
        self.result.startTestRun()
        self.result.startTest(test)
        self.result.stopTest(test)
        self.reset_output()
        self.result.stopTestRun()
        self.assertThat(self.getvalue(),
            DocTestMatches("Ran 1 test in ...s\n\nOK\n", doctest.ELLIPSIS))

    def test_stopTestRun_count_zero(self):
        self.result.startTestRun()
        self.reset_output()
        self.result.stopTestRun()
        self.assertThat(self.getvalue(),
            DocTestMatches("Ran 0 tests in ...s\n\nOK\n", doctest.ELLIPSIS))

    def test_stopTestRun_current_time(self):
        test = self.make_test()
        now = datetime.datetime.now(utc)
        self.result.time(now)
        self.result.startTestRun()
        self.result.startTest(test)
        now = now + datetime.timedelta(0, 0, 0, 1)
        self.result.time(now)
        self.result.stopTest(test)
        self.reset_output()
        self.result.stopTestRun()
        self.assertThat(self.getvalue(),
            DocTestMatches("... in 0.001s\n...", doctest.ELLIPSIS))

    def test_stopTestRun_successful(self):
        self.result.startTestRun()
        self.result.stopTestRun()
        self.assertThat(self.getvalue(),
            DocTestMatches("...\n\nOK\n", doctest.ELLIPSIS))

    def test_stopTestRun_not_successful_failure(self):
        test = self.make_failing_test()
        self.result.startTestRun()
        test.run(self.result)
        self.result.stopTestRun()
        self.assertThat(self.getvalue(),
            DocTestMatches("...\n\nFAILED (failures=1)\n", doctest.ELLIPSIS))

    def test_stopTestRun_not_successful_error(self):
        test = self.make_erroring_test()
        self.result.startTestRun()
        test.run(self.result)
        self.result.stopTestRun()
        self.assertThat(self.getvalue(),
            DocTestMatches("...\n\nFAILED (failures=1)\n", doctest.ELLIPSIS))

    def test_stopTestRun_not_successful_unexpected_success(self):
        test = self.make_unexpectedly_successful_test()
        self.result.startTestRun()
        test.run(self.result)
        self.result.stopTestRun()
        self.assertThat(self.getvalue(),
            DocTestMatches("...\n\nFAILED (failures=1)\n", doctest.ELLIPSIS))

    def test_stopTestRun_shows_details(self):
        self.result.startTestRun()
        self.make_erroring_test().run(self.result)
        self.make_unexpectedly_successful_test().run(self.result)
        self.make_failing_test().run(self.result)
        self.reset_output()
        self.result.stopTestRun()
        self.assertThat(self.getvalue(),
            DocTestMatches("""...======================================================================
ERROR: testtools.tests.test_testresult.Test.error
----------------------------------------------------------------------
Text attachment: traceback
------------
Traceback (most recent call last):
  File "...testtools...runtest.py", line ..., in _run_user...
    return fn(*args, **kwargs)
  File "...testtools...testcase.py", line ..., in _run_test_method
    return self._get_test_method()()
  File "...testtools...tests...test_testresult.py", line ..., in error
    1/0
ZeroDivisionError:... divi... by zero...
------------
======================================================================
FAIL: testtools.tests.test_testresult.Test.failed
----------------------------------------------------------------------
Text attachment: traceback
------------
Traceback (most recent call last):
  File "...testtools...runtest.py", line ..., in _run_user...
    return fn(*args, **kwargs)
  File "...testtools...testcase.py", line ..., in _run_test_method
    return self._get_test_method()()
  File "...testtools...tests...test_testresult.py", line ..., in failed
    self.fail("yo!")
AssertionError: yo!
------------
======================================================================
UNEXPECTED SUCCESS: testtools.tests.test_testresult.Test.succeeded
----------------------------------------------------------------------
...""", doctest.ELLIPSIS | doctest.REPORT_NDIFF))


class TestThreadSafeForwardingResult(TestWithFakeExceptions):
    """Tests for `TestThreadSafeForwardingResult`."""

    def setUp(self):
        TestWithFakeExceptions.setUp(self)
        self.result_semaphore = threading.Semaphore(1)
        self.target = LoggingResult([])
        self.result1 = ThreadsafeForwardingResult(self.target,
            self.result_semaphore)

    def test_nonforwarding_methods(self):
        # startTest and stopTest are not forwarded because they need to be
        # batched.
        self.result1.startTest(self)
        self.result1.stopTest(self)
        self.assertEqual([], self.target._events)

    def test_startTestRun(self):
        self.result1.startTestRun()
        self.result2 = ThreadsafeForwardingResult(self.target,
            self.result_semaphore)
        self.result2.startTestRun()
        self.assertEqual(["startTestRun", "startTestRun"], self.target._events)

    def test_stopTestRun(self):
        self.result1.stopTestRun()
        self.result2 = ThreadsafeForwardingResult(self.target,
            self.result_semaphore)
        self.result2.stopTestRun()
        self.assertEqual(["stopTestRun", "stopTestRun"], self.target._events)

    def test_forwarding_methods(self):
        # error, failure, skip and success are forwarded in batches.
        exc_info1 = self.makeExceptionInfo(RuntimeError, 'error')
        starttime1 = datetime.datetime.utcfromtimestamp(1.489)
        endtime1 = datetime.datetime.utcfromtimestamp(51.476)
        self.result1.time(starttime1)
        self.result1.startTest(self)
        self.result1.time(endtime1)
        self.result1.addError(self, exc_info1)
        exc_info2 = self.makeExceptionInfo(AssertionError, 'failure')
        starttime2 = datetime.datetime.utcfromtimestamp(2.489)
        endtime2 = datetime.datetime.utcfromtimestamp(3.476)
        self.result1.time(starttime2)
        self.result1.startTest(self)
        self.result1.time(endtime2)
        self.result1.addFailure(self, exc_info2)
        reason = _u("Skipped for some reason")
        starttime3 = datetime.datetime.utcfromtimestamp(4.489)
        endtime3 = datetime.datetime.utcfromtimestamp(5.476)
        self.result1.time(starttime3)
        self.result1.startTest(self)
        self.result1.time(endtime3)
        self.result1.addSkip(self, reason)
        starttime4 = datetime.datetime.utcfromtimestamp(6.489)
        endtime4 = datetime.datetime.utcfromtimestamp(7.476)
        self.result1.time(starttime4)
        self.result1.startTest(self)
        self.result1.time(endtime4)
        self.result1.addSuccess(self)
        self.assertEqual([
            ('time', starttime1),
            ('startTest', self),
            ('time', endtime1),
            ('addError', self, exc_info1),
            ('stopTest', self),
            ('time', starttime2),
            ('startTest', self),
            ('time', endtime2),
            ('addFailure', self, exc_info2),
            ('stopTest', self),
            ('time', starttime3),
            ('startTest', self),
            ('time', endtime3),
            ('addSkip', self, reason),
            ('stopTest', self),
            ('time', starttime4),
            ('startTest', self),
            ('time', endtime4),
            ('addSuccess', self),
            ('stopTest', self),
            ], self.target._events)


class TestExtendedToOriginalResultDecoratorBase(TestCase):

    def make_26_result(self):
        self.result = Python26TestResult()
        self.make_converter()

    def make_27_result(self):
        self.result = Python27TestResult()
        self.make_converter()

    def make_converter(self):
        self.converter = ExtendedToOriginalDecorator(self.result)

    def make_extended_result(self):
        self.result = ExtendedTestResult()
        self.make_converter()

    def check_outcome_details(self, outcome):
        """Call an outcome with a details dict to be passed through."""
        # This dict is /not/ convertible - thats deliberate, as it should
        # not hit the conversion code path.
        details = {'foo': 'bar'}
        getattr(self.converter, outcome)(self, details=details)
        self.assertEqual([(outcome, self, details)], self.result._events)

    def get_details_and_string(self):
        """Get a details dict and expected string."""
        text1 = lambda: [_b("1\n2\n")]
        text2 = lambda: [_b("3\n4\n")]
        bin1 = lambda: [_b("5\n")]
        details = {'text 1': Content(ContentType('text', 'plain'), text1),
            'text 2': Content(ContentType('text', 'strange'), text2),
            'bin 1': Content(ContentType('application', 'binary'), bin1)}
        return (details, "Binary content: bin 1\n"
            "Text attachment: text 1\n------------\n1\n2\n"
            "------------\nText attachment: text 2\n------------\n"
            "3\n4\n------------\n")

    def check_outcome_details_to_exec_info(self, outcome, expected=None):
        """Call an outcome with a details dict to be made into exc_info."""
        # The conversion is a done using RemoteError and the string contents
        # of the text types in the details dict.
        if not expected:
            expected = outcome
        details, err_str = self.get_details_and_string()
        getattr(self.converter, outcome)(self, details=details)
        err = self.converter._details_to_exc_info(details)
        self.assertEqual([(expected, self, err)], self.result._events)

    def check_outcome_details_to_nothing(self, outcome, expected=None):
        """Call an outcome with a details dict to be swallowed."""
        if not expected:
            expected = outcome
        details = {'foo': 'bar'}
        getattr(self.converter, outcome)(self, details=details)
        self.assertEqual([(expected, self)], self.result._events)

    def check_outcome_details_to_string(self, outcome):
        """Call an outcome with a details dict to be stringified."""
        details, err_str = self.get_details_and_string()
        getattr(self.converter, outcome)(self, details=details)
        self.assertEqual([(outcome, self, err_str)], self.result._events)

    def check_outcome_details_to_arg(self, outcome, arg, extra_detail=None):
        """Call an outcome with a details dict to have an arg extracted."""
        details, _ = self.get_details_and_string()
        if extra_detail:
            details.update(extra_detail)
        getattr(self.converter, outcome)(self, details=details)
        self.assertEqual([(outcome, self, arg)], self.result._events)

    def check_outcome_exc_info(self, outcome, expected=None):
        """Check that calling a legacy outcome still works."""
        # calling some outcome with the legacy exc_info style api (no keyword
        # parameters) gets passed through.
        if not expected:
            expected = outcome
        err = sys.exc_info()
        getattr(self.converter, outcome)(self, err)
        self.assertEqual([(expected, self, err)], self.result._events)

    def check_outcome_exc_info_to_nothing(self, outcome, expected=None):
        """Check that calling a legacy outcome on a fallback works."""
        # calling some outcome with the legacy exc_info style api (no keyword
        # parameters) gets passed through.
        if not expected:
            expected = outcome
        err = sys.exc_info()
        getattr(self.converter, outcome)(self, err)
        self.assertEqual([(expected, self)], self.result._events)

    def check_outcome_nothing(self, outcome, expected=None):
        """Check that calling a legacy outcome still works."""
        if not expected:
            expected = outcome
        getattr(self.converter, outcome)(self)
        self.assertEqual([(expected, self)], self.result._events)

    def check_outcome_string_nothing(self, outcome, expected):
        """Check that calling outcome with a string calls expected."""
        getattr(self.converter, outcome)(self, "foo")
        self.assertEqual([(expected, self)], self.result._events)

    def check_outcome_string(self, outcome):
        """Check that calling outcome with a string works."""
        getattr(self.converter, outcome)(self, "foo")
        self.assertEqual([(outcome, self, "foo")], self.result._events)


class TestExtendedToOriginalResultDecorator(
    TestExtendedToOriginalResultDecoratorBase):

    def test_progress_py26(self):
        self.make_26_result()
        self.converter.progress(1, 2)

    def test_progress_py27(self):
        self.make_27_result()
        self.converter.progress(1, 2)

    def test_progress_pyextended(self):
        self.make_extended_result()
        self.converter.progress(1, 2)
        self.assertEqual([('progress', 1, 2)], self.result._events)

    def test_shouldStop(self):
        self.make_26_result()
        self.assertEqual(False, self.converter.shouldStop)
        self.converter.decorated.stop()
        self.assertEqual(True, self.converter.shouldStop)

    def test_startTest_py26(self):
        self.make_26_result()
        self.converter.startTest(self)
        self.assertEqual([('startTest', self)], self.result._events)

    def test_startTest_py27(self):
        self.make_27_result()
        self.converter.startTest(self)
        self.assertEqual([('startTest', self)], self.result._events)

    def test_startTest_pyextended(self):
        self.make_extended_result()
        self.converter.startTest(self)
        self.assertEqual([('startTest', self)], self.result._events)

    def test_startTestRun_py26(self):
        self.make_26_result()
        self.converter.startTestRun()
        self.assertEqual([], self.result._events)

    def test_startTestRun_py27(self):
        self.make_27_result()
        self.converter.startTestRun()
        self.assertEqual([('startTestRun',)], self.result._events)

    def test_startTestRun_pyextended(self):
        self.make_extended_result()
        self.converter.startTestRun()
        self.assertEqual([('startTestRun',)], self.result._events)

    def test_stopTest_py26(self):
        self.make_26_result()
        self.converter.stopTest(self)
        self.assertEqual([('stopTest', self)], self.result._events)

    def test_stopTest_py27(self):
        self.make_27_result()
        self.converter.stopTest(self)
        self.assertEqual([('stopTest', self)], self.result._events)

    def test_stopTest_pyextended(self):
        self.make_extended_result()
        self.converter.stopTest(self)
        self.assertEqual([('stopTest', self)], self.result._events)

    def test_stopTestRun_py26(self):
        self.make_26_result()
        self.converter.stopTestRun()
        self.assertEqual([], self.result._events)

    def test_stopTestRun_py27(self):
        self.make_27_result()
        self.converter.stopTestRun()
        self.assertEqual([('stopTestRun',)], self.result._events)

    def test_stopTestRun_pyextended(self):
        self.make_extended_result()
        self.converter.stopTestRun()
        self.assertEqual([('stopTestRun',)], self.result._events)

    def test_tags_py26(self):
        self.make_26_result()
        self.converter.tags(1, 2)

    def test_tags_py27(self):
        self.make_27_result()
        self.converter.tags(1, 2)

    def test_tags_pyextended(self):
        self.make_extended_result()
        self.converter.tags(1, 2)
        self.assertEqual([('tags', 1, 2)], self.result._events)

    def test_time_py26(self):
        self.make_26_result()
        self.converter.time(1)

    def test_time_py27(self):
        self.make_27_result()
        self.converter.time(1)

    def test_time_pyextended(self):
        self.make_extended_result()
        self.converter.time(1)
        self.assertEqual([('time', 1)], self.result._events)


class TestExtendedToOriginalAddError(TestExtendedToOriginalResultDecoratorBase):

    outcome = 'addError'

    def test_outcome_Original_py26(self):
        self.make_26_result()
        self.check_outcome_exc_info(self.outcome)

    def test_outcome_Original_py27(self):
        self.make_27_result()
        self.check_outcome_exc_info(self.outcome)

    def test_outcome_Original_pyextended(self):
        self.make_extended_result()
        self.check_outcome_exc_info(self.outcome)

    def test_outcome_Extended_py26(self):
        self.make_26_result()
        self.check_outcome_details_to_exec_info(self.outcome)

    def test_outcome_Extended_py27(self):
        self.make_27_result()
        self.check_outcome_details_to_exec_info(self.outcome)

    def test_outcome_Extended_pyextended(self):
        self.make_extended_result()
        self.check_outcome_details(self.outcome)

    def test_outcome__no_details(self):
        self.make_extended_result()
        self.assertThat(
            lambda: getattr(self.converter, self.outcome)(self),
            Raises(MatchesException(ValueError)))


class TestExtendedToOriginalAddFailure(
    TestExtendedToOriginalAddError):

    outcome = 'addFailure'


class TestExtendedToOriginalAddExpectedFailure(
    TestExtendedToOriginalAddError):

    outcome = 'addExpectedFailure'

    def test_outcome_Original_py26(self):
        self.make_26_result()
        self.check_outcome_exc_info_to_nothing(self.outcome, 'addSuccess')

    def test_outcome_Extended_py26(self):
        self.make_26_result()
        self.check_outcome_details_to_nothing(self.outcome, 'addSuccess')



class TestExtendedToOriginalAddSkip(
    TestExtendedToOriginalResultDecoratorBase):

    outcome = 'addSkip'

    def test_outcome_Original_py26(self):
        self.make_26_result()
        self.check_outcome_string_nothing(self.outcome, 'addSuccess')

    def test_outcome_Original_py27(self):
        self.make_27_result()
        self.check_outcome_string(self.outcome)

    def test_outcome_Original_pyextended(self):
        self.make_extended_result()
        self.check_outcome_string(self.outcome)

    def test_outcome_Extended_py26(self):
        self.make_26_result()
        self.check_outcome_string_nothing(self.outcome, 'addSuccess')

    def test_outcome_Extended_py27_no_reason(self):
        self.make_27_result()
        self.check_outcome_details_to_string(self.outcome)

    def test_outcome_Extended_py27_reason(self):
        self.make_27_result()
        self.check_outcome_details_to_arg(self.outcome, 'foo',
            {'reason': Content(UTF8_TEXT, lambda:[_b('foo')])})

    def test_outcome_Extended_pyextended(self):
        self.make_extended_result()
        self.check_outcome_details(self.outcome)

    def test_outcome__no_details(self):
        self.make_extended_result()
        self.assertThat(
            lambda: getattr(self.converter, self.outcome)(self),
            Raises(MatchesException(ValueError)))


class TestExtendedToOriginalAddSuccess(
    TestExtendedToOriginalResultDecoratorBase):

    outcome = 'addSuccess'
    expected = 'addSuccess'

    def test_outcome_Original_py26(self):
        self.make_26_result()
        self.check_outcome_nothing(self.outcome, self.expected)

    def test_outcome_Original_py27(self):
        self.make_27_result()
        self.check_outcome_nothing(self.outcome)

    def test_outcome_Original_pyextended(self):
        self.make_extended_result()
        self.check_outcome_nothing(self.outcome)

    def test_outcome_Extended_py26(self):
        self.make_26_result()
        self.check_outcome_details_to_nothing(self.outcome, self.expected)

    def test_outcome_Extended_py27(self):
        self.make_27_result()
        self.check_outcome_details_to_nothing(self.outcome)

    def test_outcome_Extended_pyextended(self):
        self.make_extended_result()
        self.check_outcome_details(self.outcome)


class TestExtendedToOriginalAddUnexpectedSuccess(
    TestExtendedToOriginalResultDecoratorBase):

    outcome = 'addUnexpectedSuccess'
    expected = 'addFailure'

    def test_outcome_Original_py26(self):
        self.make_26_result()
        getattr(self.converter, self.outcome)(self)
        [event] = self.result._events
        self.assertEqual((self.expected, self), event[:2])

    def test_outcome_Original_py27(self):
        self.make_27_result()
        self.check_outcome_nothing(self.outcome)

    def test_outcome_Original_pyextended(self):
        self.make_extended_result()
        self.check_outcome_nothing(self.outcome)

    def test_outcome_Extended_py26(self):
        self.make_26_result()
        getattr(self.converter, self.outcome)(self)
        [event] = self.result._events
        self.assertEqual((self.expected, self), event[:2])

    def test_outcome_Extended_py27(self):
        self.make_27_result()
        self.check_outcome_details_to_nothing(self.outcome)

    def test_outcome_Extended_pyextended(self):
        self.make_extended_result()
        self.check_outcome_details(self.outcome)


class TestExtendedToOriginalResultOtherAttributes(
    TestExtendedToOriginalResultDecoratorBase):

    def test_other_attribute(self):
        class OtherExtendedResult:
            def foo(self):
                return 2
            bar = 1
        self.result = OtherExtendedResult()
        self.make_converter()
        self.assertEqual(1, self.converter.bar)
        self.assertEqual(2, self.converter.foo())


class TestNonAsciiResults(TestCase):
    """Test all kinds of tracebacks are cleanly interpreted as unicode

    Currently only uses weak "contains" assertions, would be good to be much
    stricter about the expected output. This would add a few failures for the
    current release of IronPython for instance, which gets some traceback
    lines muddled.
    """

    _sample_texts = (
        _u("pa\u026a\u03b8\u0259n"), # Unicode encodings only
        _u("\u5357\u7121"), # In ISO 2022 encodings
        _u("\xa7\xa7\xa7"), # In ISO 8859 encodings
        )
    # Everything but Jython shows syntax errors on the current character
    _error_on_character = os.name != "java"

    def _run(self, stream, test):
        """Run the test, the same as in testtools.run but not to stdout"""
        result = TextTestResult(stream)
        result.startTestRun()
        try:
            return test.run(result)
        finally:
            result.stopTestRun()

    def _write_module(self, name, encoding, contents):
        """Create Python module on disk with contents in given encoding"""
        try:
            # Need to pre-check that the coding is valid or codecs.open drops
            # the file without closing it which breaks non-refcounted pythons
            codecs.lookup(encoding)
        except LookupError:
            self.skip("Encoding unsupported by implementation: %r" % encoding)
        f = codecs.open(os.path.join(self.dir, name + ".py"), "w", encoding)
        try:
            f.write(contents)
        finally:
            f.close()

    def _test_external_case(self, testline, coding="ascii", modulelevel="",
            suffix=""):
        """Create and run a test case in a seperate module"""
        self._setup_external_case(testline, coding, modulelevel, suffix)
        return self._run_external_case()

    def _setup_external_case(self, testline, coding="ascii", modulelevel="",
            suffix=""):
        """Create a test case in a seperate module"""
        _, prefix, self.modname = self.id().rsplit(".", 2)
        self.dir = tempfile.mkdtemp(prefix=prefix, suffix=suffix)
        self.addCleanup(shutil.rmtree, self.dir)
        self._write_module(self.modname, coding,
            # Older Python 2 versions don't see a coding declaration in a
            # docstring so it has to be in a comment, but then we can't
            # workaround bug: <http://ironpython.codeplex.com/workitem/26940>
            "# coding: %s\n"
            "import testtools\n"
            "%s\n"
            "class Test(testtools.TestCase):\n"
            "    def runTest(self):\n"
            "        %s\n" % (coding, modulelevel, testline))

    def _run_external_case(self):
        """Run the prepared test case in a seperate module"""
        sys.path.insert(0, self.dir)
        self.addCleanup(sys.path.remove, self.dir)
        module = __import__(self.modname)
        self.addCleanup(sys.modules.pop, self.modname)
        stream = StringIO()
        self._run(stream, module.Test())
        return stream.getvalue()

    def _silence_deprecation_warnings(self):
        """Shut up DeprecationWarning for this test only"""
        warnings.simplefilter("ignore", DeprecationWarning)
        self.addCleanup(warnings.filters.remove, warnings.filters[0])

    def _get_sample_text(self, encoding="unicode_internal"):
        if encoding is None and str_is_unicode:
           encoding = "unicode_internal"
        for u in self._sample_texts:
            try:
                b = u.encode(encoding)
                if u == b.decode(encoding):
                   if str_is_unicode:
                       return u, u
                   return u, b
            except (LookupError, UnicodeError):
                pass
        self.skip("Could not find a sample text for encoding: %r" % encoding)

    def _as_output(self, text):
        return text

    def test_non_ascii_failure_string(self):
        """Assertion contents can be non-ascii and should get decoded"""
        text, raw = self._get_sample_text(_get_exception_encoding())
        textoutput = self._test_external_case("self.fail(%s)" % _r(raw))
        self.assertIn(self._as_output(text), textoutput)

    def test_non_ascii_failure_string_via_exec(self):
        """Assertion via exec can be non-ascii and still gets decoded"""
        text, raw = self._get_sample_text(_get_exception_encoding())
        textoutput = self._test_external_case(
            testline='exec ("self.fail(%s)")' % _r(raw))
        self.assertIn(self._as_output(text), textoutput)

    def test_control_characters_in_failure_string(self):
        """Control characters in assertions should be escaped"""
        textoutput = self._test_external_case("self.fail('\\a\\a\\a')")
        self.expectFailure("Defense against the beeping horror unimplemented",
            self.assertNotIn, self._as_output("\a\a\a"), textoutput)
        self.assertIn(self._as_output(_u("\uFFFD\uFFFD\uFFFD")), textoutput)

    def test_os_error(self):
        """Locale error messages from the OS shouldn't break anything"""
        textoutput = self._test_external_case(
            modulelevel="import os",
            testline="os.mkdir('/')")
        if os.name != "nt" or sys.version_info < (2, 5):
            self.assertIn(self._as_output("OSError: "), textoutput)
        else:
            self.assertIn(self._as_output("WindowsError: "), textoutput)

    def test_assertion_text_shift_jis(self):
        """A terminal raw backslash in an encoded string is weird but fine"""
        example_text = _u("\u5341")
        textoutput = self._test_external_case(
            coding="shift_jis",
            testline="self.fail('%s')" % example_text)
        if str_is_unicode:
            output_text = example_text
        else:
            output_text = example_text.encode("shift_jis").decode(
                _get_exception_encoding(), "replace")
        self.assertIn(self._as_output("AssertionError: %s" % output_text),
            textoutput)

    def test_file_comment_iso2022_jp(self):
        """Control character escapes must be preserved if valid encoding"""
        example_text, _ = self._get_sample_text("iso2022_jp")
        textoutput = self._test_external_case(
            coding="iso2022_jp",
            testline="self.fail('Simple') # %s" % example_text)
        self.assertIn(self._as_output(example_text), textoutput)

    def test_unicode_exception(self):
        """Exceptions that can be formated losslessly as unicode should be"""
        example_text, _ = self._get_sample_text()
        exception_class = (
            "class FancyError(Exception):\n"
            # A __unicode__ method does nothing on py3k but the default works
            "    def __unicode__(self):\n"
            "        return self.args[0]\n")
        textoutput = self._test_external_case(
            modulelevel=exception_class,
            testline="raise FancyError(%s)" % _r(example_text))
        self.assertIn(self._as_output(example_text), textoutput)

    def test_unprintable_exception(self):
        """A totally useless exception instance still prints something"""
        exception_class = (
            "class UnprintableError(Exception):\n"
            "    def __str__(self):\n"
            "        raise RuntimeError\n"
            "    def __unicode__(self):\n"
            "        raise RuntimeError\n"
            "    def __repr__(self):\n"
            "        raise RuntimeError\n")
        textoutput = self._test_external_case(
            modulelevel=exception_class,
            testline="raise UnprintableError")
        self.assertIn(self._as_output(
            "UnprintableError: <unprintable UnprintableError object>\n"),
            textoutput)

    def test_string_exception(self):
        """Raise a string rather than an exception instance if supported"""
        if sys.version_info > (2, 6):
            self.skip("No string exceptions in Python 2.6 or later")
        elif sys.version_info > (2, 5):
            self._silence_deprecation_warnings()
        textoutput = self._test_external_case(testline="raise 'plain str'")
        self.assertIn(self._as_output("\nplain str\n"), textoutput)

    def test_non_ascii_dirname(self):
        """Script paths in the traceback can be non-ascii"""
        text, raw = self._get_sample_text(sys.getfilesystemencoding())
        textoutput = self._test_external_case(
            # Avoid bug in Python 3 by giving a unicode source encoding rather
            # than just ascii which raises a SyntaxError with no other details
            coding="utf-8",
            testline="self.fail('Simple')",
            suffix=raw)
        self.assertIn(self._as_output(text), textoutput)

    def test_syntax_error(self):
        """Syntax errors should still have fancy special-case formatting"""
        textoutput = self._test_external_case("exec ('f(a, b c)')")
        self.assertIn(self._as_output(
            '  File "<string>", line 1\n'
            '    f(a, b c)\n'
            + ' ' * self._error_on_character +
            '          ^\n'
            'SyntaxError: '
            ), textoutput)

    def test_syntax_error_malformed(self):
        """Syntax errors with bogus parameters should break anything"""
        textoutput = self._test_external_case("raise SyntaxError(3, 2, 1)")
        self.assertIn(self._as_output("\nSyntaxError: "), textoutput)

    def test_syntax_error_import_binary(self):
        """Importing a binary file shouldn't break SyntaxError formatting"""
        if sys.version_info < (2, 5):
            # Python 2.4 assumes the file is latin-1 and tells you off
            self._silence_deprecation_warnings()
        self._setup_external_case("import bad")
        f = open(os.path.join(self.dir, "bad.py"), "wb")
        try:
            f.write(_b("x\x9c\xcb*\xcd\xcb\x06\x00\x04R\x01\xb9"))
        finally:
            f.close()
        textoutput = self._run_external_case()
        self.assertIn(self._as_output("\nSyntaxError: "), textoutput)

    def test_syntax_error_line_iso_8859_1(self):
        """Syntax error on a latin-1 line shows the line decoded"""
        text, raw = self._get_sample_text("iso-8859-1")
        textoutput = self._setup_external_case("import bad")
        self._write_module("bad", "iso-8859-1",
            "# coding: iso-8859-1\n! = 0 # %s\n" % text)
        textoutput = self._run_external_case()
        self.assertIn(self._as_output(_u(
            #'bad.py", line 2\n'
            '    ! = 0 # %s\n'
            '    ^\n'
            'SyntaxError: ') %
            (text,)), textoutput)

    def test_syntax_error_line_iso_8859_5(self):
        """Syntax error on a iso-8859-5 line shows the line decoded"""
        text, raw = self._get_sample_text("iso-8859-5")
        textoutput = self._setup_external_case("import bad")
        self._write_module("bad", "iso-8859-5",
            "# coding: iso-8859-5\n%% = 0 # %s\n" % text)
        textoutput = self._run_external_case()
        self.assertIn(self._as_output(_u(
            #'bad.py", line 2\n'
            '    %% = 0 # %s\n'
            + ' ' * self._error_on_character +
            '   ^\n'
            'SyntaxError: ') %
            (text,)), textoutput)

    def test_syntax_error_line_euc_jp(self):
        """Syntax error on a euc_jp line shows the line decoded"""
        text, raw = self._get_sample_text("euc_jp")
        textoutput = self._setup_external_case("import bad")
        self._write_module("bad", "euc_jp",
            "# coding: euc_jp\n$ = 0 # %s\n" % text)
        textoutput = self._run_external_case()
        self.assertIn(self._as_output(_u(
            #'bad.py", line 2\n'
            '    $ = 0 # %s\n'
            + ' ' * self._error_on_character +
            '   ^\n'
            'SyntaxError: ') %
            (text,)), textoutput)

    def test_syntax_error_line_utf_8(self):
        """Syntax error on a utf-8 line shows the line decoded"""
        text, raw = self._get_sample_text("utf-8")
        textoutput = self._setup_external_case("import bad")
        self._write_module("bad", "utf-8", _u("\ufeff^ = 0 # %s\n") % text)
        textoutput = self._run_external_case()
        self.assertIn(self._as_output(_u(
            'bad.py", line 1\n'
            '    ^ = 0 # %s\n'
            + ' ' * self._error_on_character +
            '   ^\n'
            'SyntaxError: ') %
            text), textoutput)


class TestNonAsciiResultsWithUnittest(TestNonAsciiResults):
    """Test that running under unittest produces clean ascii strings"""

    def _run(self, stream, test):
        from unittest import TextTestRunner as _Runner
        return _Runner(stream).run(test)

    def _as_output(self, text):
        if str_is_unicode:
            return text
        return text.encode("utf-8")


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
