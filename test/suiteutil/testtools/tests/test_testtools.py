# Copyright (c) 2008-2010 testtools developers. See LICENSE for details.

"""Tests for extensions to the base test library."""

from pprint import pformat
import os
import sys
import tempfile
import unittest

from testtools import (
    ErrorHolder,
    MultipleExceptions,
    PlaceHolder,
    TestCase,
    clone_test_with_new_id,
    content,
    skip,
    skipIf,
    skipUnless,
    testcase,
    )
from testtools.matchers import (
    Equals,
    MatchesException,
    Raises,
    )
from testtools.tests.helpers import (
    an_exc_info,
    LoggingResult,
    Python26TestResult,
    Python27TestResult,
    ExtendedTestResult,
    )
try:
    exec('from __future__ import with_statement')
except SyntaxError:
    pass
else:
    from testtools.tests.test_with_with import *


class TestPlaceHolder(TestCase):

    def makePlaceHolder(self, test_id="foo", short_description=None):
        return PlaceHolder(test_id, short_description)

    def test_id_comes_from_constructor(self):
        # The id() of a PlaceHolder is whatever you pass into the constructor.
        test = PlaceHolder("test id")
        self.assertEqual("test id", test.id())

    def test_shortDescription_is_id(self):
        # The shortDescription() of a PlaceHolder is the id, by default.
        test = PlaceHolder("test id")
        self.assertEqual(test.id(), test.shortDescription())

    def test_shortDescription_specified(self):
        # If a shortDescription is provided to the constructor, then
        # shortDescription() returns that instead.
        test = PlaceHolder("test id", "description")
        self.assertEqual("description", test.shortDescription())

    def test_repr_just_id(self):
        # repr(placeholder) shows you how the object was constructed.
        test = PlaceHolder("test id")
        self.assertEqual(
            "<testtools.testcase.PlaceHolder(%s)>" % repr(test.id()),
            repr(test))

    def test_repr_with_description(self):
        # repr(placeholder) shows you how the object was constructed.
        test = PlaceHolder("test id", "description")
        self.assertEqual(
            "<testtools.testcase.PlaceHolder(%r, %r)>" % (
                test.id(), test.shortDescription()),
            repr(test))

    def test_counts_as_one_test(self):
        # A placeholder test counts as one test.
        test = self.makePlaceHolder()
        self.assertEqual(1, test.countTestCases())

    def test_str_is_id(self):
        # str(placeholder) is always the id(). We are not barbarians.
        test = self.makePlaceHolder()
        self.assertEqual(test.id(), str(test))

    def test_runs_as_success(self):
        # When run, a PlaceHolder test records a success.
        test = self.makePlaceHolder()
        log = []
        test.run(LoggingResult(log))
        self.assertEqual(
            [('startTest', test), ('addSuccess', test), ('stopTest', test)],
            log)

    def test_call_is_run(self):
        # A PlaceHolder can be called, in which case it behaves like run.
        test = self.makePlaceHolder()
        run_log = []
        test.run(LoggingResult(run_log))
        call_log = []
        test(LoggingResult(call_log))
        self.assertEqual(run_log, call_log)

    def test_runs_without_result(self):
        # A PlaceHolder can be run without a result, in which case there's no
        # way to actually get at the result.
        self.makePlaceHolder().run()

    def test_debug(self):
        # A PlaceHolder can be debugged.
        self.makePlaceHolder().debug()


class TestErrorHolder(TestCase):

    def makeException(self):
        try:
            raise RuntimeError("danger danger")
        except:
            return sys.exc_info()

    def makePlaceHolder(self, test_id="foo", error=None,
                        short_description=None):
        if error is None:
            error = self.makeException()
        return ErrorHolder(test_id, error, short_description)

    def test_id_comes_from_constructor(self):
        # The id() of a PlaceHolder is whatever you pass into the constructor.
        test = ErrorHolder("test id", self.makeException())
        self.assertEqual("test id", test.id())

    def test_shortDescription_is_id(self):
        # The shortDescription() of a PlaceHolder is the id, by default.
        test = ErrorHolder("test id", self.makeException())
        self.assertEqual(test.id(), test.shortDescription())

    def test_shortDescription_specified(self):
        # If a shortDescription is provided to the constructor, then
        # shortDescription() returns that instead.
        test = ErrorHolder("test id", self.makeException(), "description")
        self.assertEqual("description", test.shortDescription())

    def test_repr_just_id(self):
        # repr(placeholder) shows you how the object was constructed.
        error = self.makeException()
        test = ErrorHolder("test id", error)
        self.assertEqual(
            "<testtools.testcase.ErrorHolder(%r, %r)>" % (test.id(), error),
            repr(test))

    def test_repr_with_description(self):
        # repr(placeholder) shows you how the object was constructed.
        error = self.makeException()
        test = ErrorHolder("test id", error, "description")
        self.assertEqual(
            "<testtools.testcase.ErrorHolder(%r, %r, %r)>" % (
                test.id(), error, test.shortDescription()),
            repr(test))

    def test_counts_as_one_test(self):
        # A placeholder test counts as one test.
        test = self.makePlaceHolder()
        self.assertEqual(1, test.countTestCases())

    def test_str_is_id(self):
        # str(placeholder) is always the id(). We are not barbarians.
        test = self.makePlaceHolder()
        self.assertEqual(test.id(), str(test))

    def test_runs_as_error(self):
        # When run, a PlaceHolder test records a success.
        error = self.makeException()
        test = self.makePlaceHolder(error=error)
        log = []
        test.run(LoggingResult(log))
        self.assertEqual(
            [('startTest', test),
             ('addError', test, error),
             ('stopTest', test)], log)

    def test_call_is_run(self):
        # A PlaceHolder can be called, in which case it behaves like run.
        test = self.makePlaceHolder()
        run_log = []
        test.run(LoggingResult(run_log))
        call_log = []
        test(LoggingResult(call_log))
        self.assertEqual(run_log, call_log)

    def test_runs_without_result(self):
        # A PlaceHolder can be run without a result, in which case there's no
        # way to actually get at the result.
        self.makePlaceHolder().run()

    def test_debug(self):
        # A PlaceHolder can be debugged.
        self.makePlaceHolder().debug()


class TestEquality(TestCase):
    """Test ``TestCase``'s equality implementation."""

    def test_identicalIsEqual(self):
        # TestCase's are equal if they are identical.
        self.assertEqual(self, self)

    def test_nonIdenticalInUnequal(self):
        # TestCase's are not equal if they are not identical.
        self.assertNotEqual(TestCase(methodName='run'),
            TestCase(methodName='skip'))


class TestAssertions(TestCase):
    """Test assertions in TestCase."""

    def raiseError(self, exceptionFactory, *args, **kwargs):
        raise exceptionFactory(*args, **kwargs)

    def test_formatTypes_single(self):
        # Given a single class, _formatTypes returns the name.
        class Foo(object):
            pass
        self.assertEqual('Foo', self._formatTypes(Foo))

    def test_formatTypes_multiple(self):
        # Given multiple types, _formatTypes returns the names joined by
        # commas.
        class Foo(object):
            pass
        class Bar(object):
            pass
        self.assertEqual('Foo, Bar', self._formatTypes([Foo, Bar]))

    def test_assertRaises(self):
        # assertRaises asserts that a callable raises a particular exception.
        self.assertRaises(RuntimeError, self.raiseError, RuntimeError)

    def test_assertRaises_fails_when_no_error_raised(self):
        # assertRaises raises self.failureException when it's passed a
        # callable that raises no error.
        ret = ('orange', 42)
        try:
            self.assertRaises(RuntimeError, lambda: ret)
        except self.failureException:
            # We expected assertRaises to raise this exception.
            e = sys.exc_info()[1]
            self.assertEqual(
                '%s not raised, %r returned instead.'
                % (self._formatTypes(RuntimeError), ret), str(e))
        else:
            self.fail('Expected assertRaises to fail, but it did not.')

    def test_assertRaises_fails_when_different_error_raised(self):
        # assertRaises re-raises an exception that it didn't expect.
        self.assertThat(lambda: self.assertRaises(RuntimeError,
            self.raiseError, ZeroDivisionError),
            Raises(MatchesException(ZeroDivisionError)))

    def test_assertRaises_returns_the_raised_exception(self):
        # assertRaises returns the exception object that was raised. This is
        # useful for testing that exceptions have the right message.

        # This contraption stores the raised exception, so we can compare it
        # to the return value of assertRaises.
        raisedExceptions = []
        def raiseError():
            try:
                raise RuntimeError('Deliberate error')
            except RuntimeError:
                raisedExceptions.append(sys.exc_info()[1])
                raise

        exception = self.assertRaises(RuntimeError, raiseError)
        self.assertEqual(1, len(raisedExceptions))
        self.assertTrue(
            exception is raisedExceptions[0],
            "%r is not %r" % (exception, raisedExceptions[0]))

    def test_assertRaises_with_multiple_exceptions(self):
        # assertRaises((ExceptionOne, ExceptionTwo), function) asserts that
        # function raises one of ExceptionTwo or ExceptionOne.
        expectedExceptions = (RuntimeError, ZeroDivisionError)
        self.assertRaises(
            expectedExceptions, self.raiseError, expectedExceptions[0])
        self.assertRaises(
            expectedExceptions, self.raiseError, expectedExceptions[1])

    def test_assertRaises_with_multiple_exceptions_failure_mode(self):
        # If assertRaises is called expecting one of a group of exceptions and
        # a callable that doesn't raise an exception, then fail with an
        # appropriate error message.
        expectedExceptions = (RuntimeError, ZeroDivisionError)
        failure = self.assertRaises(
            self.failureException,
            self.assertRaises, expectedExceptions, lambda: None)
        self.assertEqual(
            '%s not raised, None returned instead.'
            % self._formatTypes(expectedExceptions), str(failure))

    def assertFails(self, message, function, *args, **kwargs):
        """Assert that function raises a failure with the given message."""
        failure = self.assertRaises(
            self.failureException, function, *args, **kwargs)
        self.assertEqual(message, str(failure))

    def test_assertIn_success(self):
        # assertIn(needle, haystack) asserts that 'needle' is in 'haystack'.
        self.assertIn(3, range(10))
        self.assertIn('foo', 'foo bar baz')
        self.assertIn('foo', 'foo bar baz'.split())

    def test_assertIn_failure(self):
        # assertIn(needle, haystack) fails the test when 'needle' is not in
        # 'haystack'.
        self.assertFails('3 not in [0, 1, 2]', self.assertIn, 3, [0, 1, 2])
        self.assertFails(
            '%r not in %r' % ('qux', 'foo bar baz'),
            self.assertIn, 'qux', 'foo bar baz')

    def test_assertNotIn_success(self):
        # assertNotIn(needle, haystack) asserts that 'needle' is not in
        # 'haystack'.
        self.assertNotIn(3, [0, 1, 2])
        self.assertNotIn('qux', 'foo bar baz')

    def test_assertNotIn_failure(self):
        # assertNotIn(needle, haystack) fails the test when 'needle' is in
        # 'haystack'.
        self.assertFails('3 in [1, 2, 3]', self.assertNotIn, 3, [1, 2, 3])
        self.assertFails(
            '%r in %r' % ('foo', 'foo bar baz'),
            self.assertNotIn, 'foo', 'foo bar baz')

    def test_assertIsInstance(self):
        # assertIsInstance asserts that an object is an instance of a class.

        class Foo(object):
            """Simple class for testing assertIsInstance."""

        foo = Foo()
        self.assertIsInstance(foo, Foo)

    def test_assertIsInstance_multiple_classes(self):
        # assertIsInstance asserts that an object is an instance of one of a
        # group of classes.

        class Foo(object):
            """Simple class for testing assertIsInstance."""

        class Bar(object):
            """Another simple class for testing assertIsInstance."""

        foo = Foo()
        self.assertIsInstance(foo, (Foo, Bar))
        self.assertIsInstance(Bar(), (Foo, Bar))

    def test_assertIsInstance_failure(self):
        # assertIsInstance(obj, klass) fails the test when obj is not an
        # instance of klass.

        class Foo(object):
            """Simple class for testing assertIsInstance."""

        self.assertFails(
            '42 is not an instance of %s' % self._formatTypes(Foo),
            self.assertIsInstance, 42, Foo)

    def test_assertIsInstance_failure_multiple_classes(self):
        # assertIsInstance(obj, (klass1, klass2)) fails the test when obj is
        # not an instance of klass1 or klass2.

        class Foo(object):
            """Simple class for testing assertIsInstance."""

        class Bar(object):
            """Another simple class for testing assertIsInstance."""

        self.assertFails(
            '42 is not an instance of %s' % self._formatTypes([Foo, Bar]),
            self.assertIsInstance, 42, (Foo, Bar))

    def test_assertIsInstance_overridden_message(self):
        # assertIsInstance(obj, klass, msg) permits a custom message.
        self.assertFails("foo", self.assertIsInstance, 42, str, "foo")

    def test_assertIs(self):
        # assertIs asserts that an object is identical to another object.
        self.assertIs(None, None)
        some_list = [42]
        self.assertIs(some_list, some_list)
        some_object = object()
        self.assertIs(some_object, some_object)

    def test_assertIs_fails(self):
        # assertIs raises assertion errors if one object is not identical to
        # another.
        self.assertFails('None is not 42', self.assertIs, None, 42)
        self.assertFails('[42] is not [42]', self.assertIs, [42], [42])

    def test_assertIs_fails_with_message(self):
        # assertIs raises assertion errors if one object is not identical to
        # another, and includes a user-supplied message, if it's provided.
        self.assertFails(
            'None is not 42: foo bar', self.assertIs, None, 42, 'foo bar')

    def test_assertIsNot(self):
        # assertIsNot asserts that an object is not identical to another
        # object.
        self.assertIsNot(None, 42)
        self.assertIsNot([42], [42])
        self.assertIsNot(object(), object())

    def test_assertIsNot_fails(self):
        # assertIsNot raises assertion errors if one object is identical to
        # another.
        self.assertFails('None is None', self.assertIsNot, None, None)
        some_list = [42]
        self.assertFails(
            '[42] is [42]', self.assertIsNot, some_list, some_list)

    def test_assertIsNot_fails_with_message(self):
        # assertIsNot raises assertion errors if one object is identical to
        # another, and includes a user-supplied message if it's provided.
        self.assertFails(
            'None is None: foo bar', self.assertIsNot, None, None, "foo bar")

    def test_assertThat_matches_clean(self):
        class Matcher(object):
            def match(self, foo):
                return None
        self.assertThat("foo", Matcher())

    def test_assertThat_mismatch_raises_description(self):
        calls = []
        class Mismatch(object):
            def __init__(self, thing):
                self.thing = thing
            def describe(self):
                calls.append(('describe_diff', self.thing))
                return "object is not a thing"
            def get_details(self):
                return {}
        class Matcher(object):
            def match(self, thing):
                calls.append(('match', thing))
                return Mismatch(thing)
            def __str__(self):
                calls.append(('__str__',))
                return "a description"
        class Test(TestCase):
            def test(self):
                self.assertThat("foo", Matcher())
        result = Test("test").run()
        self.assertEqual([
            ('match', "foo"),
            ('describe_diff', "foo"),
            ('__str__',),
            ], calls)
        self.assertFalse(result.wasSuccessful())

    def test_assertEqual_nice_formatting(self):
        message = "These things ought not be equal."
        a = ['apple', 'banana', 'cherry']
        b = {'Thatcher': 'One who mends roofs of straw',
             'Major': 'A military officer, ranked below colonel',
             'Blair': 'To shout loudly',
             'Brown': 'The colour of healthy human faeces'}
        expected_error = '\n'.join(
            [message,
             'not equal:',
             'a = %s' % pformat(a),
             'b = %s' % pformat(b),
             ''])
        expected_error = '\n'.join([
            'Match failed. Matchee: "%r"' % b,
            'Matcher: Annotate(%r, Equals(%r))' % (message, a),
            'Difference: !=:',
            'reference = %s' % pformat(a),
            'actual = %s' % pformat(b),
            ': ' + message,
            ''
            ])
        self.assertFails(expected_error, self.assertEqual, a, b, message)
        self.assertFails(expected_error, self.assertEquals, a, b, message)
        self.assertFails(expected_error, self.failUnlessEqual, a, b, message)

    def test_assertEqual_formatting_no_message(self):
        a = "cat"
        b = "dog"
        expected_error = '\n'.join([
            'Match failed. Matchee: "dog"',
            'Matcher: Equals(\'cat\')',
            'Difference: \'cat\' != \'dog\'',
            ''
            ])
        self.assertFails(expected_error, self.assertEqual, a, b)
        self.assertFails(expected_error, self.assertEquals, a, b)
        self.assertFails(expected_error, self.failUnlessEqual, a, b)


class TestAddCleanup(TestCase):
    """Tests for TestCase.addCleanup."""

    class LoggingTest(TestCase):
        """A test that logs calls to setUp, runTest and tearDown."""

        def setUp(self):
            TestCase.setUp(self)
            self._calls = ['setUp']

        def brokenSetUp(self):
            # A tearDown that deliberately fails.
            self._calls = ['brokenSetUp']
            raise RuntimeError('Deliberate Failure')

        def runTest(self):
            self._calls.append('runTest')

        def brokenTest(self):
            raise RuntimeError('Deliberate broken test')

        def tearDown(self):
            self._calls.append('tearDown')
            TestCase.tearDown(self)

    def setUp(self):
        TestCase.setUp(self)
        self._result_calls = []
        self.test = TestAddCleanup.LoggingTest('runTest')
        self.logging_result = LoggingResult(self._result_calls)

    def assertErrorLogEqual(self, messages):
        self.assertEqual(messages, [call[0] for call in self._result_calls])

    def assertTestLogEqual(self, messages):
        """Assert that the call log equals 'messages'."""
        case = self._result_calls[0][1]
        self.assertEqual(messages, case._calls)

    def logAppender(self, message):
        """A cleanup that appends 'message' to the tests log.

        Cleanups are callables that are added to a test by addCleanup. To
        verify that our cleanups run in the right order, we add strings to a
        list that acts as a log. This method returns a cleanup that will add
        the given message to that log when run.
        """
        self.test._calls.append(message)

    def test_fixture(self):
        # A normal run of self.test logs 'setUp', 'runTest' and 'tearDown'.
        # This test doesn't test addCleanup itself, it just sanity checks the
        # fixture.
        self.test.run(self.logging_result)
        self.assertTestLogEqual(['setUp', 'runTest', 'tearDown'])

    def test_cleanup_run_before_tearDown(self):
        # Cleanup functions added with 'addCleanup' are called before tearDown
        # runs.
        self.test.addCleanup(self.logAppender, 'cleanup')
        self.test.run(self.logging_result)
        self.assertTestLogEqual(['setUp', 'runTest', 'tearDown', 'cleanup'])

    def test_add_cleanup_called_if_setUp_fails(self):
        # Cleanup functions added with 'addCleanup' are called even if setUp
        # fails. Note that tearDown has a different behavior: it is only
        # called when setUp succeeds.
        self.test.setUp = self.test.brokenSetUp
        self.test.addCleanup(self.logAppender, 'cleanup')
        self.test.run(self.logging_result)
        self.assertTestLogEqual(['brokenSetUp', 'cleanup'])

    def test_addCleanup_called_in_reverse_order(self):
        # Cleanup functions added with 'addCleanup' are called in reverse
        # order.
        #
        # One of the main uses of addCleanup is to dynamically create
        # resources that need some sort of explicit tearDown. Often one
        # resource will be created in terms of another, e.g.,
        #     self.first = self.makeFirst()
        #     self.second = self.makeSecond(self.first)
        #
        # When this happens, we generally want to clean up the second resource
        # before the first one, since the second depends on the first.
        self.test.addCleanup(self.logAppender, 'first')
        self.test.addCleanup(self.logAppender, 'second')
        self.test.run(self.logging_result)
        self.assertTestLogEqual(
            ['setUp', 'runTest', 'tearDown', 'second', 'first'])

    def test_tearDown_runs_after_cleanup_failure(self):
        # tearDown runs even if a cleanup function fails.
        self.test.addCleanup(lambda: 1/0)
        self.test.run(self.logging_result)
        self.assertTestLogEqual(['setUp', 'runTest', 'tearDown'])

    def test_cleanups_continue_running_after_error(self):
        # All cleanups are always run, even if one or two of them fail.
        self.test.addCleanup(self.logAppender, 'first')
        self.test.addCleanup(lambda: 1/0)
        self.test.addCleanup(self.logAppender, 'second')
        self.test.run(self.logging_result)
        self.assertTestLogEqual(
            ['setUp', 'runTest', 'tearDown', 'second', 'first'])

    def test_error_in_cleanups_are_captured(self):
        # If a cleanup raises an error, we want to record it and fail the the
        # test, even though we go on to run other cleanups.
        self.test.addCleanup(lambda: 1/0)
        self.test.run(self.logging_result)
        self.assertErrorLogEqual(['startTest', 'addError', 'stopTest'])

    def test_keyboard_interrupt_not_caught(self):
        # If a cleanup raises KeyboardInterrupt, it gets reraised.
        def raiseKeyboardInterrupt():
            raise KeyboardInterrupt()
        self.test.addCleanup(raiseKeyboardInterrupt)
        self.assertThat(lambda:self.test.run(self.logging_result),
            Raises(MatchesException(KeyboardInterrupt)))

    def test_all_errors_from_MultipleExceptions_reported(self):
        # When a MultipleExceptions exception is caught, all the errors are
        # reported.
        def raiseMany():
            try:
                1/0
            except Exception:
                exc_info1 = sys.exc_info()
            try:
                1/0
            except Exception:
                exc_info2 = sys.exc_info()
            raise MultipleExceptions(exc_info1, exc_info2)
        self.test.addCleanup(raiseMany)
        self.logging_result = ExtendedTestResult()
        self.test.run(self.logging_result)
        self.assertEqual(['startTest', 'addError', 'stopTest'],
            [event[0] for event in self.logging_result._events])
        self.assertEqual(set(['traceback', 'traceback-1']),
            set(self.logging_result._events[1][2].keys()))

    def test_multipleCleanupErrorsReported(self):
        # Errors from all failing cleanups are reported as separate backtraces.
        self.test.addCleanup(lambda: 1/0)
        self.test.addCleanup(lambda: 1/0)
        self.logging_result = ExtendedTestResult()
        self.test.run(self.logging_result)
        self.assertEqual(['startTest', 'addError', 'stopTest'],
            [event[0] for event in self.logging_result._events])
        self.assertEqual(set(['traceback', 'traceback-1']),
            set(self.logging_result._events[1][2].keys()))

    def test_multipleErrorsCoreAndCleanupReported(self):
        # Errors from all failing cleanups are reported, with stopTest,
        # startTest inserted.
        self.test = TestAddCleanup.LoggingTest('brokenTest')
        self.test.addCleanup(lambda: 1/0)
        self.test.addCleanup(lambda: 1/0)
        self.logging_result = ExtendedTestResult()
        self.test.run(self.logging_result)
        self.assertEqual(['startTest', 'addError', 'stopTest'],
            [event[0] for event in self.logging_result._events])
        self.assertEqual(set(['traceback', 'traceback-1', 'traceback-2']),
            set(self.logging_result._events[1][2].keys()))


class TestWithDetails(TestCase):

    def assertDetailsProvided(self, case, expected_outcome, expected_keys):
        """Assert that when case is run, details are provided to the result.

        :param case: A TestCase to run.
        :param expected_outcome: The call that should be made.
        :param expected_keys: The keys to look for.
        """
        result = ExtendedTestResult()
        case.run(result)
        case = result._events[0][1]
        expected = [
            ('startTest', case),
            (expected_outcome, case),
            ('stopTest', case),
            ]
        self.assertEqual(3, len(result._events))
        self.assertEqual(expected[0], result._events[0])
        self.assertEqual(expected[1], result._events[1][0:2])
        # Checking the TB is right is rather tricky. doctest line matching
        # would help, but 'meh'.
        self.assertEqual(sorted(expected_keys),
            sorted(result._events[1][2].keys()))
        self.assertEqual(expected[-1], result._events[-1])

    def get_content(self):
        return content.Content(
            content.ContentType("text", "foo"), lambda: ['foo'])


class TestExpectedFailure(TestWithDetails):
    """Tests for expected failures and unexpected successess."""

    def make_unexpected_case(self):
        class Case(TestCase):
            def test(self):
                raise testcase._UnexpectedSuccess
        case = Case('test')
        return case

    def test_raising__UnexpectedSuccess_py27(self):
        case = self.make_unexpected_case()
        result = Python27TestResult()
        case.run(result)
        case = result._events[0][1]
        self.assertEqual([
            ('startTest', case),
            ('addUnexpectedSuccess', case),
            ('stopTest', case),
            ], result._events)

    def test_raising__UnexpectedSuccess_extended(self):
        case = self.make_unexpected_case()
        result = ExtendedTestResult()
        case.run(result)
        case = result._events[0][1]
        self.assertEqual([
            ('startTest', case),
            ('addUnexpectedSuccess', case, {}),
            ('stopTest', case),
            ], result._events)

    def make_xfail_case_xfails(self):
        content = self.get_content()
        class Case(TestCase):
            def test(self):
                self.addDetail("foo", content)
                self.expectFailure("we are sad", self.assertEqual,
                    1, 0)
        case = Case('test')
        return case

    def make_xfail_case_succeeds(self):
        content = self.get_content()
        class Case(TestCase):
            def test(self):
                self.addDetail("foo", content)
                self.expectFailure("we are sad", self.assertEqual,
                    1, 1)
        case = Case('test')
        return case

    def test_expectFailure_KnownFailure_extended(self):
        case = self.make_xfail_case_xfails()
        self.assertDetailsProvided(case, "addExpectedFailure",
            ["foo", "traceback", "reason"])

    def test_expectFailure_KnownFailure_unexpected_success(self):
        case = self.make_xfail_case_succeeds()
        self.assertDetailsProvided(case, "addUnexpectedSuccess",
            ["foo", "reason"])


class TestUniqueFactories(TestCase):
    """Tests for getUniqueString and getUniqueInteger."""

    def test_getUniqueInteger(self):
        # getUniqueInteger returns an integer that increments each time you
        # call it.
        one = self.getUniqueInteger()
        self.assertEqual(1, one)
        two = self.getUniqueInteger()
        self.assertEqual(2, two)

    def test_getUniqueString(self):
        # getUniqueString returns the current test id followed by a unique
        # integer.
        name_one = self.getUniqueString()
        self.assertEqual('%s-%d' % (self.id(), 1), name_one)
        name_two = self.getUniqueString()
        self.assertEqual('%s-%d' % (self.id(), 2), name_two)

    def test_getUniqueString_prefix(self):
        # If getUniqueString is given an argument, it uses that argument as
        # the prefix of the unique string, rather than the test id.
        name_one = self.getUniqueString('foo')
        self.assertThat(name_one, Equals('foo-1'))
        name_two = self.getUniqueString('bar')
        self.assertThat(name_two, Equals('bar-2'))


class TestCloneTestWithNewId(TestCase):
    """Tests for clone_test_with_new_id."""

    def test_clone_test_with_new_id(self):
        class FooTestCase(TestCase):
            def test_foo(self):
                pass
        test = FooTestCase('test_foo')
        oldName = test.id()
        newName = self.getUniqueString()
        newTest = clone_test_with_new_id(test, newName)
        self.assertEqual(newName, newTest.id())
        self.assertEqual(oldName, test.id(),
            "the original test instance should be unchanged.")

    def test_cloned_testcase_does_not_share_details(self):
        """A cloned TestCase does not share the details dict."""
        class Test(TestCase):
            def test_foo(self):
                self.addDetail(
                    'foo', content.Content('text/plain', lambda: 'foo'))
        orig_test = Test('test_foo')
        cloned_test = clone_test_with_new_id(orig_test, self.getUniqueString())
        orig_test.run(unittest.TestResult())
        self.assertEqual('foo', orig_test.getDetails()['foo'].iter_bytes())
        self.assertEqual(None, cloned_test.getDetails().get('foo'))


class TestDetailsProvided(TestWithDetails):

    def test_addDetail(self):
        mycontent = self.get_content()
        self.addDetail("foo", mycontent)
        details = self.getDetails()
        self.assertEqual({"foo": mycontent}, details)

    def test_addError(self):
        class Case(TestCase):
            def test(this):
                this.addDetail("foo", self.get_content())
                1/0
        self.assertDetailsProvided(Case("test"), "addError",
            ["foo", "traceback"])

    def test_addFailure(self):
        class Case(TestCase):
            def test(this):
                this.addDetail("foo", self.get_content())
                self.fail('yo')
        self.assertDetailsProvided(Case("test"), "addFailure",
            ["foo", "traceback"])

    def test_addSkip(self):
        class Case(TestCase):
            def test(this):
                this.addDetail("foo", self.get_content())
                self.skip('yo')
        self.assertDetailsProvided(Case("test"), "addSkip",
            ["foo", "reason"])

    def test_addSucccess(self):
        class Case(TestCase):
            def test(this):
                this.addDetail("foo", self.get_content())
        self.assertDetailsProvided(Case("test"), "addSuccess",
            ["foo"])

    def test_addUnexpectedSuccess(self):
        class Case(TestCase):
            def test(this):
                this.addDetail("foo", self.get_content())
                raise testcase._UnexpectedSuccess()
        self.assertDetailsProvided(Case("test"), "addUnexpectedSuccess",
            ["foo"])

    def test_addDetails_from_Mismatch(self):
        content = self.get_content()
        class Mismatch(object):
            def describe(self):
                return "Mismatch"
            def get_details(self):
                return {"foo": content}
        class Matcher(object):
            def match(self, thing):
                return Mismatch()
            def __str__(self):
                return "a description"
        class Case(TestCase):
            def test(self):
                self.assertThat("foo", Matcher())
        self.assertDetailsProvided(Case("test"), "addFailure",
            ["foo", "traceback"])

    def test_multiple_addDetails_from_Mismatch(self):
        content = self.get_content()
        class Mismatch(object):
            def describe(self):
                return "Mismatch"
            def get_details(self):
                return {"foo": content, "bar": content}
        class Matcher(object):
            def match(self, thing):
                return Mismatch()
            def __str__(self):
                return "a description"
        class Case(TestCase):
            def test(self):
                self.assertThat("foo", Matcher())
        self.assertDetailsProvided(Case("test"), "addFailure",
            ["bar", "foo", "traceback"])

    def test_addDetails_with_same_name_as_key_from_get_details(self):
        content = self.get_content()
        class Mismatch(object):
            def describe(self):
                return "Mismatch"
            def get_details(self):
                return {"foo": content}
        class Matcher(object):
            def match(self, thing):
                return Mismatch()
            def __str__(self):
                return "a description"
        class Case(TestCase):
            def test(self):
                self.addDetail("foo", content)
                self.assertThat("foo", Matcher())
        self.assertDetailsProvided(Case("test"), "addFailure",
            ["foo", "foo-1", "traceback"])


class TestSetupTearDown(TestCase):

    def test_setUpNotCalled(self):
        class DoesnotcallsetUp(TestCase):
            def setUp(self):
                pass
            def test_method(self):
                pass
        result = unittest.TestResult()
        DoesnotcallsetUp('test_method').run(result)
        self.assertEqual(1, len(result.errors))

    def test_tearDownNotCalled(self):
        class DoesnotcalltearDown(TestCase):
            def test_method(self):
                pass
            def tearDown(self):
                pass
        result = unittest.TestResult()
        DoesnotcalltearDown('test_method').run(result)
        self.assertEqual(1, len(result.errors))


class TestSkipping(TestCase):
    """Tests for skipping of tests functionality."""

    def test_skip_causes_skipException(self):
        self.assertThat(lambda:self.skip("Skip this test"),
            Raises(MatchesException(self.skipException)))

    def test_can_use_skipTest(self):
        self.assertThat(lambda:self.skipTest("Skip this test"),
            Raises(MatchesException(self.skipException)))

    def test_skip_without_reason_works(self):
        class Test(TestCase):
            def test(self):
                raise self.skipException()
        case = Test("test")
        result = ExtendedTestResult()
        case.run(result)
        self.assertEqual('addSkip', result._events[1][0])
        self.assertEqual('no reason given.',
            ''.join(result._events[1][2]['reason'].iter_text()))

    def test_skipException_in_setup_calls_result_addSkip(self):
        class TestThatRaisesInSetUp(TestCase):
            def setUp(self):
                TestCase.setUp(self)
                self.skip("skipping this test")
            def test_that_passes(self):
                pass
        calls = []
        result = LoggingResult(calls)
        test = TestThatRaisesInSetUp("test_that_passes")
        test.run(result)
        case = result._events[0][1]
        self.assertEqual([('startTest', case),
            ('addSkip', case, "skipping this test"), ('stopTest', case)],
            calls)

    def test_skipException_in_test_method_calls_result_addSkip(self):
        class SkippingTest(TestCase):
            def test_that_raises_skipException(self):
                self.skip("skipping this test")
        result = Python27TestResult()
        test = SkippingTest("test_that_raises_skipException")
        test.run(result)
        case = result._events[0][1]
        self.assertEqual([('startTest', case),
            ('addSkip', case, "skipping this test"), ('stopTest', case)],
            result._events)

    def test_skip__in_setup_with_old_result_object_calls_addSuccess(self):
        class SkippingTest(TestCase):
            def setUp(self):
                TestCase.setUp(self)
                raise self.skipException("skipping this test")
            def test_that_raises_skipException(self):
                pass
        result = Python26TestResult()
        test = SkippingTest("test_that_raises_skipException")
        test.run(result)
        self.assertEqual('addSuccess', result._events[1][0])

    def test_skip_with_old_result_object_calls_addError(self):
        class SkippingTest(TestCase):
            def test_that_raises_skipException(self):
                raise self.skipException("skipping this test")
        result = Python26TestResult()
        test = SkippingTest("test_that_raises_skipException")
        test.run(result)
        self.assertEqual('addSuccess', result._events[1][0])

    def test_skip_decorator(self):
        class SkippingTest(TestCase):
            @skip("skipping this test")
            def test_that_is_decorated_with_skip(self):
                self.fail()
        result = Python26TestResult()
        test = SkippingTest("test_that_is_decorated_with_skip")
        test.run(result)
        self.assertEqual('addSuccess', result._events[1][0])

    def test_skipIf_decorator(self):
        class SkippingTest(TestCase):
            @skipIf(True, "skipping this test")
            def test_that_is_decorated_with_skipIf(self):
                self.fail()
        result = Python26TestResult()
        test = SkippingTest("test_that_is_decorated_with_skipIf")
        test.run(result)
        self.assertEqual('addSuccess', result._events[1][0])

    def test_skipUnless_decorator(self):
        class SkippingTest(TestCase):
            @skipUnless(False, "skipping this test")
            def test_that_is_decorated_with_skipUnless(self):
                self.fail()
        result = Python26TestResult()
        test = SkippingTest("test_that_is_decorated_with_skipUnless")
        test.run(result)
        self.assertEqual('addSuccess', result._events[1][0])


class TestOnException(TestCase):

    def test_default_works(self):
        events = []
        class Case(TestCase):
            def method(self):
                self.onException(an_exc_info)
                events.append(True)
        case = Case("method")
        case.run()
        self.assertThat(events, Equals([True]))

    def test_added_handler_works(self):
        events = []
        class Case(TestCase):
            def method(self):
                self.addOnException(events.append)
                self.onException(an_exc_info)
        case = Case("method")
        case.run()
        self.assertThat(events, Equals([an_exc_info]))

    def test_handler_that_raises_is_not_caught(self):
        events = []
        class Case(TestCase):
            def method(self):
                self.addOnException(events.index)
                self.assertThat(lambda: self.onException(an_exc_info),
                    Raises(MatchesException(ValueError)))
        case = Case("method")
        case.run()
        self.assertThat(events, Equals([]))


class TestPatchSupport(TestCase):

    class Case(TestCase):
        def test(self):
            pass

    def test_patch(self):
        # TestCase.patch masks obj.attribute with the new value.
        self.foo = 'original'
        test = self.Case('test')
        test.patch(self, 'foo', 'patched')
        self.assertEqual('patched', self.foo)

    def test_patch_restored_after_run(self):
        # TestCase.patch masks obj.attribute with the new value, but restores
        # the original value after the test is finished.
        self.foo = 'original'
        test = self.Case('test')
        test.patch(self, 'foo', 'patched')
        test.run()
        self.assertEqual('original', self.foo)

    def test_successive_patches_apply(self):
        # TestCase.patch can be called multiple times per test. Each time you
        # call it, it overrides the original value.
        self.foo = 'original'
        test = self.Case('test')
        test.patch(self, 'foo', 'patched')
        test.patch(self, 'foo', 'second')
        self.assertEqual('second', self.foo)

    def test_successive_patches_restored_after_run(self):
        # TestCase.patch restores the original value, no matter how many times
        # it was called.
        self.foo = 'original'
        test = self.Case('test')
        test.patch(self, 'foo', 'patched')
        test.patch(self, 'foo', 'second')
        test.run()
        self.assertEqual('original', self.foo)

    def test_patch_nonexistent_attribute(self):
        # TestCase.patch can be used to patch a non-existent attribute.
        test = self.Case('test')
        test.patch(self, 'doesntexist', 'patched')
        self.assertEqual('patched', self.doesntexist)

    def test_restore_nonexistent_attribute(self):
        # TestCase.patch can be used to patch a non-existent attribute, after
        # the test run, the attribute is then removed from the object.
        test = self.Case('test')
        test.patch(self, 'doesntexist', 'patched')
        test.run()
        marker = object()
        value = getattr(self, 'doesntexist', marker)
        self.assertIs(marker, value)


class TestTestCaseSuper(TestCase):
    def test_setup_uses_super(self):
        class OtherBaseCase(unittest.TestCase):
            setup_called = False
            def setUp(self):
                self.setup_called = True
                super(OtherBaseCase, self).setUp()
        class OurCase(TestCase, OtherBaseCase):
            def runTest(self):
                pass
        test = OurCase()
        test.setUp()
        test.tearDown()
        self.assertTrue(test.setup_called)

    def test_teardown_uses_super(self):
        class OtherBaseCase(unittest.TestCase):
            teardown_called = False
            def tearDown(self):
                self.teardown_called = True
                super(OtherBaseCase, self).tearDown()
        class OurCase(TestCase, OtherBaseCase):
            def runTest(self):
                pass
        test = OurCase()
        test.setUp()
        test.tearDown()
        self.assertTrue(test.teardown_called)


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
