# Copyright (c) 2008-2015 testtools developers. See LICENSE for details.

"""Tests for extensions to the base test library."""

from doctest import ELLIPSIS
from pprint import pformat
import sys
import _thread
import unittest

from testtools import (
    DecorateTestCaseResult,
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
from testtools.compat import (
    _b,
    )
from testtools.content import (
    text_content,
    TracebackContent,
    )
from testtools.matchers import (
    Annotate,
    ContainsAll,
    DocTestMatches,
    Equals,
    HasLength,
    MatchesException,
    Raises,
    )
from testtools.testcase import (
    attr,
    Nullary,
    WithAttributes,
    TestSkipped,
    )
from testtools.testresult.doubles import (
    Python26TestResult,
    Python27TestResult,
    ExtendedTestResult,
    )
from testtools.tests.helpers import (
    an_exc_info,
    AsText,
    FullStackRunTest,
    LoggingResult,
    MatchesEvents,
    raise_,
    )
from testtools.tests.samplecases import (
    deterministic_sample_cases_scenarios,
    make_case_for_behavior_scenario,
    make_test_case,
    nondeterministic_sample_cases_scenarios,
)


class TestPlaceHolder(TestCase):

    run_test_with = FullStackRunTest

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

    def test_testcase_is_hashable(self):
        test = hash(self)
        self.assertEqual(unittest.TestCase.__hash__(self), test)

    def test_testcase_equals_edgecase(self):
        # Not all python objects have a __dict__ attribute
        self.assertFalse(self == _thread.RLock())

    def test_repr_just_id(self):
        # repr(placeholder) shows you how the object was constructed.
        test = PlaceHolder("test id")
        self.assertEqual(
            "<testtools.testcase.PlaceHolder('addSuccess', %s, {})>" % repr(
            test.id()), repr(test))

    def test_repr_with_description(self):
        # repr(placeholder) shows you how the object was constructed.
        test = PlaceHolder("test id", "description")
        self.assertEqual(
            "<testtools.testcase.PlaceHolder('addSuccess', {!r}, {{}}, {!r})>".format(
            test.id(), test.shortDescription()), repr(test))

    def test_repr_custom_outcome(self):
        test = PlaceHolder("test id", outcome='addSkip')
        self.assertEqual(
            "<testtools.testcase.PlaceHolder('addSkip', %r, {})>" % (
            test.id()), repr(test))

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
            [('tags', set(), set()), ('startTest', test), ('addSuccess', test),
             ('stopTest', test), ('tags', set(), set()),],
            log)

    def test_supplies_details(self):
        details = {'quux':None}
        test = PlaceHolder('foo', details=details)
        result = ExtendedTestResult()
        test.run(result)
        self.assertEqual(
            [('tags', set(), set()),
             ('startTest', test),
             ('addSuccess', test, details),
             ('stopTest', test),
             ('tags', set(), set()),
             ],
            result._events)

    def test_supplies_timestamps(self):
        test = PlaceHolder('foo', details={}, timestamps=["A", "B"])
        result = ExtendedTestResult()
        test.run(result)
        self.assertEqual(
            [('time', "A"),
             ('tags', set(), set()),
             ('startTest', test),
             ('time', "B"),
             ('addSuccess', test),
             ('stopTest', test),
             ('tags', set(), set()),
             ],
            result._events)

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

    def test_supports_tags(self):
        result = ExtendedTestResult()
        tags = {'foo', 'bar'}
        case = PlaceHolder("foo", tags=tags)
        case.run(result)
        self.assertEqual([
            ('tags', tags, set()),
            ('startTest', case),
            ('addSuccess', case),
            ('stopTest', case),
            ('tags', set(), tags),
            ], result._events)


class TestErrorHolder(TestCase):
    # Note that these tests exist because ErrorHolder exists - it could be
    # deprecated and dropped at this point.

    run_test_with = FullStackRunTest

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

    def test_counts_as_one_test(self):
        # A placeholder test counts as one test.
        test = self.makePlaceHolder()
        self.assertEqual(1, test.countTestCases())

    def test_str_is_id(self):
        # str(placeholder) is always the id(). We are not barbarians.
        test = self.makePlaceHolder()
        self.assertEqual(test.id(), str(test))

    def test_runs_as_error(self):
        # When run, an ErrorHolder test records an error.
        error = self.makeException()
        test = self.makePlaceHolder(error=error)
        result = ExtendedTestResult()
        log = result._events
        test.run(result)
        self.assertEqual(
            [('tags', set(), set()),
             ('startTest', test),
             ('addError', test, test._details),
             ('stopTest', test),
             ('tags', set(), set())], log)

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

    run_test_with = FullStackRunTest

    def test_identicalIsEqual(self):
        # TestCase's are equal if they are identical.
        self.assertEqual(self, self)

    def test_nonIdenticalInUnequal(self):
        # TestCase's are not equal if they are not identical.
        self.assertNotEqual(TestCase(methodName='run'),
            TestCase(methodName='skip'))


class TestAssertions(TestCase):
    """Test assertions in TestCase."""

    run_test_with = FullStackRunTest

    def raiseError(self, exceptionFactory, *args, **kwargs):
        raise exceptionFactory(*args, **kwargs)

    def test_formatTypes_single(self):
        # Given a single class, _formatTypes returns the name.
        class Foo:
            pass
        self.assertEqual('Foo', self._formatTypes(Foo))

    def test_formatTypes_multiple(self):
        # Given multiple types, _formatTypes returns the names joined by
        # commas.
        class Foo:
            pass
        class Bar:
            pass
        self.assertEqual('Foo, Bar', self._formatTypes([Foo, Bar]))

    def test_assertRaises(self):
        # assertRaises asserts that a callable raises a particular exception.
        self.assertRaises(RuntimeError, self.raiseError, RuntimeError)

    def test_assertRaises_exception_w_metaclass(self):
        # assertRaises works when called for exceptions with custom metaclasses
        class MyExMeta(type):
            def __init__(cls, name, bases, dct):
                """ Do some dummy metaclass stuff """
                dct.update({'answer': 42})
                type.__init__(cls, name, bases, dct)

        class MyEx(Exception):
            __metaclass__ = MyExMeta

        self.assertRaises(MyEx, self.raiseError, MyEx)

    def test_assertRaises_fails_when_no_error_raised(self):
        # assertRaises raises self.failureException when it's passed a
        # callable that raises no error.
        ret = ('orange', 42)
        self.assertFails(
            "<function ...<lambda> at ...> returned ('orange', 42)",
            self.assertRaises, RuntimeError, lambda: ret)

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
        self.assertIs(
            exception,
            raisedExceptions[0],
            '{!r} is not {!r}'.format(exception, raisedExceptions[0])
        )

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
        self.assertRaises(
            self.failureException,
            self.assertRaises, expectedExceptions, lambda: None)
        self.assertFails('<function ...<lambda> at ...> returned None',
            self.assertRaises, expectedExceptions, lambda: None)

    def test_assertRaises_function_repr_in_exception(self):
        # When assertRaises fails, it includes the repr of the invoked
        # function in the error message, so it's easy to locate the problem.
        def foo():
            """An arbitrary function."""
            pass
        self.assertThat(
            lambda: self.assertRaises(Exception, foo),
            Raises(
                MatchesException(self.failureException, f'.*{foo!r}.*')))

    def test_assertRaisesRegex(self):
        # assertRaisesRegex asserts that function raises particular exception
        # with particular message.
        self.assertRaisesRegex(RuntimeError, r"M\w*e", self.raiseError,
                               RuntimeError, "Message")

    def test_assertRaisesRegex_wrong_error_type(self):
        # If function raises an exception of unexpected type,
        # assertRaisesRegex re-raises it.
        self.assertRaises(ValueError, self.assertRaisesRegex, RuntimeError,
                          r"M\w*e", self.raiseError, ValueError, "Message")

    def test_assertRaisesRegex_wrong_message(self):
        # If function raises an exception with unexpected message
        # assertRaisesRegex fails.
        self.assertFails(
            '"Expected" does not match "Observed"',
            self.assertRaisesRegex, RuntimeError, "Expected",
            self.raiseError, RuntimeError, "Observed")

    def assertFails(self, message, function, *args, **kwargs):
        """Assert that function raises a failure with the given message."""
        failure = self.assertRaises(
            self.failureException, function, *args, **kwargs)
        self.assertThat(failure, DocTestMatches(message, ELLIPSIS))

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
            '{!r} not in {!r}'.format('qux', 'foo bar baz'),
            self.assertIn, 'qux', 'foo bar baz')

    def test_assertIn_failure_with_message(self):
        # assertIn(needle, haystack) fails the test when 'needle' is not in
        # 'haystack'.
        self.assertFails('3 not in [0, 1, 2]: foo bar', self.assertIn, 3,
                         [0, 1, 2], 'foo bar')
        self.assertFails(
            '{!r} not in {!r}: foo bar'.format('qux', 'foo bar baz'),
            self.assertIn, 'qux', 'foo bar baz', 'foo bar')


    def test_assertNotIn_success(self):
        # assertNotIn(needle, haystack) asserts that 'needle' is not in
        # 'haystack'.
        self.assertNotIn(3, [0, 1, 2])
        self.assertNotIn('qux', 'foo bar baz')

    def test_assertNotIn_failure(self):
        # assertNotIn(needle, haystack) fails the test when 'needle' is in
        # 'haystack'.
        self.assertFails('[1, 2, 3] matches Contains(3)', self.assertNotIn,
            3, [1, 2, 3])
        self.assertFails(
            "'foo bar baz' matches Contains('foo')",
            self.assertNotIn, 'foo', 'foo bar baz')


    def test_assertNotIn_failure_with_message(self):
        # assertNotIn(needle, haystack) fails the test when 'needle' is in
        # 'haystack'.
        self.assertFails('[1, 2, 3] matches Contains(3): foo bar', self.assertNotIn,
            3, [1, 2, 3], 'foo bar')
        self.assertFails(
            "'foo bar baz' matches Contains('foo'): foo bar",
            self.assertNotIn, 'foo', 'foo bar baz', "foo bar")



    def test_assertIsInstance(self):
        # assertIsInstance asserts that an object is an instance of a class.

        class Foo:
            """Simple class for testing assertIsInstance."""

        foo = Foo()
        self.assertIsInstance(foo, Foo)

    def test_assertIsInstance_multiple_classes(self):
        # assertIsInstance asserts that an object is an instance of one of a
        # group of classes.

        class Foo:
            """Simple class for testing assertIsInstance."""

        class Bar:
            """Another simple class for testing assertIsInstance."""

        foo = Foo()
        self.assertIsInstance(foo, (Foo, Bar))
        self.assertIsInstance(Bar(), (Foo, Bar))

    def test_assertIsInstance_failure(self):
        # assertIsInstance(obj, klass) fails the test when obj is not an
        # instance of klass.

        class Foo:
            """Simple class for testing assertIsInstance."""

        self.assertFails(
            "'42' is not an instance of %s" % self._formatTypes(Foo),
            self.assertIsInstance, 42, Foo)

    def test_assertIsInstance_failure_multiple_classes(self):
        # assertIsInstance(obj, (klass1, klass2)) fails the test when obj is
        # not an instance of klass1 or klass2.

        class Foo:
            """Simple class for testing assertIsInstance."""

        class Bar:
            """Another simple class for testing assertIsInstance."""

        self.assertFails(
            "'42' is not an instance of any of (%s)" % self._formatTypes([Foo, Bar]),
            self.assertIsInstance, 42, (Foo, Bar))

    def test_assertIsInstance_overridden_message(self):
        # assertIsInstance(obj, klass, msg) permits a custom message.
        self.assertFails("'42' is not an instance of str: foo",
            self.assertIsInstance, 42, str, "foo")

    def test_assertIs(self):
        # assertIs asserts that an object is identical to another object.
        self.assertIsNone(None)
        some_list = [42]
        self.assertIs(some_list, some_list)
        some_object = object()
        self.assertIs(some_object, some_object)

    def test_assertIs_fails(self):
        # assertIs raises assertion errors if one object is not identical to
        # another.
        self.assertFails('42 is not None', self.assertIs, None, 42)
        self.assertFails('[42] is not [42]', self.assertIs, [42], [42])

    def test_assertIs_fails_with_message(self):
        # assertIs raises assertion errors if one object is not identical to
        # another, and includes a user-supplied message, if it's provided.
        self.assertFails(
            '42 is not None: foo bar', self.assertIs, None, 42, 'foo bar')

    def test_assertIsNot(self):
        # assertIsNot asserts that an object is not identical to another
        # object.
        self.assertIsNot(None, 42)
        self.assertIsNot([42], [42])
        self.assertIsNot(object(), object())

    def test_assertIsNot_fails(self):
        # assertIsNot raises assertion errors if one object is identical to
        # another.
        self.assertFails('None matches Is(None)', self.assertIsNot, None, None)
        some_list = [42]
        self.assertFails(
            '[42] matches Is([42])', self.assertIsNot, some_list, some_list)

    def test_assertIsNot_fails_with_message(self):
        # assertIsNot raises assertion errors if one object is identical to
        # another, and includes a user-supplied message if it's provided.
        self.assertFails(
            'None matches Is(None): foo bar', self.assertIsNot, None, None,
            "foo bar")

    def test_assertThat_matches_clean(self):
        class Matcher:
            def match(self, foo):
                return None
        self.assertThat("foo", Matcher())

    def test_assertThat_mismatch_raises_description(self):
        calls = []
        class Mismatch:
            def __init__(self, thing):
                self.thing = thing
            def describe(self):
                calls.append(('describe_diff', self.thing))
                return "object is not a thing"
            def get_details(self):
                return {}
        class Matcher:
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
            ], calls)
        self.assertFalse(result.wasSuccessful())

    def test_assertThat_output(self):
        matchee = 'foo'
        matcher = Equals('bar')
        expected = matcher.match(matchee).describe()
        self.assertFails(expected, self.assertThat, matchee, matcher)

    def test_assertThat_message_is_annotated(self):
        matchee = 'foo'
        matcher = Equals('bar')
        expected = Annotate('woo', matcher).match(matchee).describe()
        self.assertFails(expected, self.assertThat, matchee, matcher, 'woo')

    def test_assertThat_verbose_output(self):
        matchee = 'foo'
        matcher = Equals('bar')
        expected = (
            'Match failed. Matchee: %r\n'
            'Matcher: %s\n'
            'Difference: %s\n' % (
                matchee,
                matcher,
                matcher.match(matchee).describe(),
                ))
        self.assertFails(
            expected, self.assertThat, matchee, matcher, verbose=True)

    def test_expectThat_matches_clean(self):
        class Matcher:
            def match(self, foo):
                return None
        self.expectThat("foo", Matcher())

    def test_expectThat_mismatch_fails_test(self):
        class Test(TestCase):
            def test(self):
                self.expectThat("foo", Equals("bar"))
        result = Test("test").run()
        self.assertFalse(result.wasSuccessful())

    def test_expectThat_does_not_exit_test(self):
        class Test(TestCase):
            marker = False
            def test(self):
                self.expectThat("foo", Equals("bar"))
                Test.marker = True
        result = Test("test").run()
        self.assertFalse(result.wasSuccessful())
        self.assertTrue(Test.marker)

    def test_expectThat_adds_detail(self):
        class Test(TestCase):
            def test(self):
                self.expectThat("foo", Equals("bar"))
        test = Test("test")
        test.run()
        details = test.getDetails()
        self.assertIn('Failed expectation', details)

    def test__force_failure_fails_test(self):
        class Test(TestCase):
            def test_foo(self):
                self.force_failure = True
                self.remaining_code_run = True
        test = Test('test_foo')
        result = test.run()
        self.assertFalse(result.wasSuccessful())
        self.assertTrue(test.remaining_code_run)

    def get_error_string(self, e):
        """Get the string showing how 'e' would be formatted in test output.

        This is a little bit hacky, since it's designed to give consistent
        output regardless of Python version.

        In testtools, TestResult._exc_info_to_unicode is the point of dispatch
        between various different implementations of methods that format
        exceptions, so that's what we have to call. However, that method cares
        about stack traces and formats the exception class. We don't care
        about either of these, so we take its output and parse it a little.
        """
        error = TracebackContent((e.__class__, e, None), self).as_text()
        # We aren't at all interested in the traceback.
        if error.startswith('Traceback (most recent call last):\n'):
            lines = error.splitlines(True)[1:]
            for i, line in enumerate(lines):
                if not line.startswith(' '):
                    break
            error = ''.join(lines[i:])
        # We aren't interested in how the exception type is formatted.
        exc_class, error = error.split(': ', 1)
        return error

    def test_assertThat_verbose_unicode(self):
        # When assertThat is given matchees or matchers that contain non-ASCII
        # unicode strings, we can still provide a meaningful error.
        matchee = '\xa7'
        matcher = Equals('a')
        expected = (
            'Match failed. Matchee: %s\n'
            'Matcher: %s\n'
            'Difference: %s\n\n' % (
                repr(matchee).replace("\\xa7", matchee),
                matcher,
                matcher.match(matchee).describe(),
                ))
        e = self.assertRaises(
            self.failureException, self.assertThat, matchee, matcher,
            verbose=True)
        self.assertEqual(expected, self.get_error_string(e))

    def test_assertEqual_nice_formatting(self):
        message = "These things ought not be equal."
        a = ['apple', 'banana', 'cherry']
        b = {'Thatcher': 'One who mends roofs of straw',
             'Major': 'A military officer, ranked below colonel',
             'Blair': 'To shout loudly',
             'Brown': 'The colour of healthy human faeces'}
        expected_error = '\n'.join([
            '!=:',
            'reference = %s' % pformat(a),
            'actual    = %s' % pformat(b),
            ': ' + message,
            ])
        self.assertFails(expected_error, self.assertEqual, a, b, message)
        self.assertFails(expected_error, self.assertEquals, a, b, message)
        self.assertFails(expected_error, self.failUnlessEqual, a, b, message)

    def test_assertEqual_formatting_no_message(self):
        a = "cat"
        b = "dog"
        expected_error = "'cat' != 'dog'"
        self.assertFails(expected_error, self.assertEqual, a, b)
        self.assertFails(expected_error, self.assertEquals, a, b)
        self.assertFails(expected_error, self.failUnlessEqual, a, b)

    def test_assertEqual_non_ascii_str_with_newlines(self):
        message = "Be careful mixing unicode and bytes"
        a = "a\n\xa7\n"
        b = "Just a longish string so the more verbose output form is used."
        expected_error = '\n'.join([
            '!=:',
            "reference = '''\\",
            'a',
            repr('\xa7')[1:-1],
            "'''",
            f'actual    = {b!r}',
            ': ' + message,
            ])
        self.assertFails(expected_error, self.assertEqual, a, b, message)

    def test_assertIsNone(self):
        self.assertIsNone(None)

        expected_error = '0 is not None'
        self.assertFails(expected_error, self.assertIsNone, 0)

    def test_assertIsNotNone(self):
        self.assertIsNotNone(0)
        self.assertIsNotNone("0")

        expected_error = 'None matches Is(None)'
        self.assertFails(expected_error, self.assertIsNotNone, None)


    def test_fail_preserves_traceback_detail(self):
        class Test(TestCase):
            def test(self):
                self.addDetail('traceback', text_content('foo'))
                self.fail('bar')
        test = Test('test')
        result = ExtendedTestResult()
        test.run(result)
        self.assertEqual({'traceback', 'traceback-1'},
            set(result._events[1][2].keys()))


class TestAddCleanup(TestCase):
    """Tests for TestCase.addCleanup."""

    run_tests_with = FullStackRunTest

    def test_cleanup_run_after_tearDown(self):
        # Cleanup functions added with 'addCleanup' are called after tearDown
        # runs.
        log = []
        test = make_test_case(
            self.getUniqueString(),
            set_up=lambda _: log.append('setUp'),
            test_body=lambda _: log.append('runTest'),
            tear_down=lambda _: log.append('tearDown'),
            cleanups=[lambda _: log.append('cleanup')],
        )
        test.run()
        self.assertThat(
            log, Equals(['setUp', 'runTest', 'tearDown', 'cleanup']))

    def test_add_cleanup_called_if_setUp_fails(self):
        # Cleanup functions added with 'addCleanup' are called even if setUp
        # fails. Note that tearDown has a different behavior: it is only
        # called when setUp succeeds.
        log = []

        def broken_set_up(ignored):
            log.append('brokenSetUp')
            raise RuntimeError('Deliberate broken setUp')

        test = make_test_case(
            self.getUniqueString(),
            set_up=broken_set_up,
            test_body=lambda _: log.append('runTest'),
            tear_down=lambda _: log.append('tearDown'),
            cleanups=[lambda _: log.append('cleanup')],
        )
        test.run()
        self.assertThat(log, Equals(['brokenSetUp', 'cleanup']))

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
        log = []
        test = make_test_case(
            self.getUniqueString(),
            set_up=lambda _: log.append('setUp'),
            test_body=lambda _: log.append('runTest'),
            tear_down=lambda _: log.append('tearDown'),
            cleanups=[
                lambda _: log.append('first'),
                lambda _: log.append('second'),
            ],
        )
        test.run()
        self.assertThat(
            log, Equals(['setUp', 'runTest', 'tearDown', 'second', 'first']))

    def test_tearDown_runs_on_cleanup_failure(self):
        # tearDown runs even if a cleanup function fails.
        log = []
        test = make_test_case(
            self.getUniqueString(),
            set_up=lambda _: log.append('setUp'),
            test_body=lambda _: log.append('runTest'),
            tear_down=lambda _: log.append('tearDown'),
            cleanups=[lambda _: 1/0],
        )
        test.run()
        self.assertThat(log, Equals(['setUp', 'runTest', 'tearDown']))

    def test_cleanups_continue_running_after_error(self):
        # All cleanups are always run, even if one or two of them fail.
        log = []
        test = make_test_case(
            self.getUniqueString(),
            set_up=lambda _: log.append('setUp'),
            test_body=lambda _: log.append('runTest'),
            tear_down=lambda _: log.append('tearDown'),
            cleanups=[
                lambda _: log.append('first'),
                lambda _: 1/0,
                lambda _: log.append('second'),
            ],
        )
        test.run()
        self.assertThat(
            log, Equals(['setUp', 'runTest', 'tearDown', 'second', 'first']))

    def test_error_in_cleanups_are_captured(self):
        # If a cleanup raises an error, we want to record it and fail the the
        # test, even though we go on to run other cleanups.
        test = make_test_case(self.getUniqueString(), cleanups=[lambda _: 1/0])
        log = []
        test.run(ExtendedTestResult(log))
        self.assertThat(
            log, MatchesEvents(
                ('startTest', test),
                ('addError', test, {
                    'traceback': AsText(ContainsAll([
                        'Traceback (most recent call last):',
                        'ZeroDivisionError',
                    ])),
                }),
                ('stopTest', test),
            )
        )

    def test_keyboard_interrupt_not_caught(self):
        # If a cleanup raises KeyboardInterrupt, it gets reraised.
        test = make_test_case(
            self.getUniqueString(), cleanups=[
                lambda _: raise_(KeyboardInterrupt())])
        self.assertThat(test.run, Raises(MatchesException(KeyboardInterrupt)))

    def test_all_errors_from_MultipleExceptions_reported(self):
        # When a MultipleExceptions exception is caught, all the errors are
        # reported.
        def raise_many(ignored):
            try:
                1/0
            except Exception:
                exc_info1 = sys.exc_info()
            try:
                1/0
            except Exception:
                exc_info2 = sys.exc_info()
            raise MultipleExceptions(exc_info1, exc_info2)

        test = make_test_case(self.getUniqueString(), cleanups=[raise_many])
        log = []
        test.run(ExtendedTestResult(log))
        self.assertThat(
            log, MatchesEvents(
                ('startTest', test),
                ('addError', test, {
                    'traceback': AsText(ContainsAll([
                        'Traceback (most recent call last):',
                        'ZeroDivisionError',
                    ])),
                    'traceback-1': AsText(ContainsAll([
                        'Traceback (most recent call last):',
                        'ZeroDivisionError',
                    ])),
                }),
                ('stopTest', test),
            )
        )

    def test_multipleCleanupErrorsReported(self):
        # Errors from all failing cleanups are reported as separate backtraces.
        test = make_test_case(self.getUniqueString(), cleanups=[
            lambda _: 1/0,
            lambda _: 1/0,
        ])
        log = []
        test.run(ExtendedTestResult(log))
        self.assertThat(
            log, MatchesEvents(
                ('startTest', test),
                ('addError', test, {
                    'traceback': AsText(ContainsAll([
                        'Traceback (most recent call last):',
                        'ZeroDivisionError',
                    ])),
                    'traceback-1': AsText(ContainsAll([
                        'Traceback (most recent call last):',
                        'ZeroDivisionError',
                    ])),
                }),
                ('stopTest', test),
            )
        )

    def test_multipleErrorsCoreAndCleanupReported(self):
        # Errors from all failing cleanups are reported, with stopTest,
        # startTest inserted.
        test = make_test_case(
            self.getUniqueString(),
            test_body=lambda _: raise_(
                RuntimeError('Deliberately broken test')),
            cleanups=[
                lambda _: 1/0,
                lambda _: 1/0,
            ]
        )
        log = []
        test.run(ExtendedTestResult(log))
        self.assertThat(
            log, MatchesEvents(
                ('startTest', test),
                ('addError', test, {
                    'traceback': AsText(ContainsAll([
                        'Traceback (most recent call last):',
                        'RuntimeError: Deliberately broken test',
                    ])),
                    'traceback-1': AsText(ContainsAll([
                        'Traceback (most recent call last):',
                        'ZeroDivisionError',
                    ])),
                    'traceback-2': AsText(ContainsAll([
                        'Traceback (most recent call last):',
                        'ZeroDivisionError',
                    ])),
                }),
                ('stopTest', test),
            )
        )


class TestRunTestUsage(TestCase):

    def test_last_resort_in_place(self):
        class TestBase(TestCase):
            def test_base_exception(self):
                raise SystemExit(0)
        result = ExtendedTestResult()
        test = TestBase("test_base_exception")
        self.assertRaises(SystemExit, test.run, result)
        self.assertFalse(result.wasSuccessful())


class TestWithDetails(TestCase):

    run_test_with = FullStackRunTest

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
            content.ContentType("text", "foo"), lambda: [_b('foo')])


class TestExpectedFailure(TestWithDetails):
    """Tests for expected failures and unexpected successess."""

    run_test_with = FullStackRunTest

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

    def test_unittest_expectedFailure_decorator_works_with_failure(self):
        class ReferenceTest(TestCase):
            @unittest.expectedFailure
            def test_fails_expectedly(self):
                self.assertEqual(1, 0)

        test = ReferenceTest('test_fails_expectedly')
        result = test.run()
        self.assertEqual(True, result.wasSuccessful())

    def test_unittest_expectedFailure_decorator_works_with_success(self):
        class ReferenceTest(TestCase):
            @unittest.expectedFailure
            def test_passes_unexpectedly(self):
                self.assertEqual(1, 1)

        test = ReferenceTest('test_passes_unexpectedly')
        result = test.run()
        self.assertEqual(False, result.wasSuccessful())


class TestUniqueFactories(TestCase):
    """Tests for getUniqueString, getUniqueInteger, unique_text_generator."""

    run_test_with = FullStackRunTest

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

    def test_unique_text_generator(self):
        # unique_text_generator yields the prefix's id followed by unique
        # unicode string.
        prefix = self.getUniqueString()
        unique_text_generator = testcase.unique_text_generator(prefix)
        first_result = next(unique_text_generator)
        self.assertEqual('{}-{}'.format(prefix, '\u1e00'),
                         first_result)
        # The next value yielded by unique_text_generator is different.
        second_result = next(unique_text_generator)
        self.assertEqual('{}-{}'.format(prefix, '\u1e01'),
                         second_result)

    def test_mods(self):
        # given a number and max, generate a list that's the mods.
        # The list should contain no numbers >= mod
        self.assertEqual([0], list(testcase._mods(0, 5)))
        self.assertEqual([1], list(testcase._mods(1, 5)))
        self.assertEqual([2], list(testcase._mods(2, 5)))
        self.assertEqual([3], list(testcase._mods(3, 5)))
        self.assertEqual([4], list(testcase._mods(4, 5)))
        self.assertEqual([0, 1], list(testcase._mods(5, 5)))
        self.assertEqual([1, 1], list(testcase._mods(6, 5)))
        self.assertEqual([2, 1], list(testcase._mods(7, 5)))
        self.assertEqual([0, 2], list(testcase._mods(10, 5)))
        self.assertEqual([0, 0, 1], list(testcase._mods(25, 5)))
        self.assertEqual([1, 0, 1], list(testcase._mods(26, 5)))
        self.assertEqual([1], list(testcase._mods(1, 100)))
        self.assertEqual([0, 1], list(testcase._mods(100, 100)))
        self.assertEqual([0, 10], list(testcase._mods(1000, 100)))

    def test_unique_text(self):
        self.assertEqual(
            '\u1e00',
            testcase._unique_text(base_cp=0x1e00, cp_range=5, index=0))
        self.assertEqual(
            '\u1e01',
            testcase._unique_text(base_cp=0x1e00, cp_range=5, index=1))
        self.assertEqual(
            '\u1e00\u1e01',
            testcase._unique_text(base_cp=0x1e00, cp_range=5, index=5))
        self.assertEqual(
            '\u1e03\u1e02\u1e01',
            testcase._unique_text(base_cp=0x1e00, cp_range=5, index=38))


class TestCloneTestWithNewId(TestCase):
    """Tests for clone_test_with_new_id."""

    run_test_with = FullStackRunTest

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

    run_test_with = FullStackRunTest

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
                self.skipTest('yo')
        self.assertDetailsProvided(Case("test"), "addSkip",
            ["foo", "reason"])

    def test_addSkip_different_exception(self):
        # No traceback is included if the skip exception is changed and a skip
        # is raised.
        class Case(TestCase):
            skipException = ValueError

            def test(this):
                this.addDetail("foo", self.get_content())
                this.skipTest('yo')
        self.assertDetailsProvided(Case("test"), "addSkip", ["foo", "reason"])

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
        class Mismatch:
            def describe(self):
                return "Mismatch"
            def get_details(self):
                return {"foo": content}
        class Matcher:
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
        class Mismatch:
            def describe(self):
                return "Mismatch"
            def get_details(self):
                return {"foo": content, "bar": content}
        class Matcher:
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
        class Mismatch:
            def describe(self):
                return "Mismatch"
            def get_details(self):
                return {"foo": content}
        class Matcher:
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

    def test_addDetailUniqueName_works(self):
        content = self.get_content()
        class Case(TestCase):
            def test(self):
                self.addDetailUniqueName("foo", content)
                self.addDetailUniqueName("foo", content)
        self.assertDetailsProvided(Case("test"), "addSuccess",
            ["foo", "foo-1"])


class TestSetupTearDown(TestCase):

    run_test_with = FullStackRunTest

    def test_setUpCalledTwice(self):
        class CallsTooMuch(TestCase):
            def test_method(self):
                self.setUp()
        result = unittest.TestResult()
        CallsTooMuch('test_method').run(result)
        self.assertThat(result.errors, HasLength(1))
        self.assertThat(result.errors[0][1],
            DocTestMatches(
                "...ValueError...File...testtools/tests/test_testcase.py...",
                ELLIPSIS))

    def test_setUpNotCalled(self):
        class DoesnotcallsetUp(TestCase):
            def setUp(self):
                pass
            def test_method(self):
                pass
        result = unittest.TestResult()
        DoesnotcallsetUp('test_method').run(result)
        self.assertThat(result.errors, HasLength(1))
        self.assertThat(result.errors[0][1],
            DocTestMatches(
                "...ValueError...File...testtools/tests/test_testcase.py...",
                ELLIPSIS))

    def test_tearDownCalledTwice(self):
        class CallsTooMuch(TestCase):
            def test_method(self):
                self.tearDown()
        result = unittest.TestResult()
        CallsTooMuch('test_method').run(result)
        self.assertThat(result.errors, HasLength(1))
        self.assertThat(result.errors[0][1],
            DocTestMatches(
                "...ValueError...File...testtools/tests/test_testcase.py...",
                ELLIPSIS))

    def test_tearDownNotCalled(self):
        class DoesnotcalltearDown(TestCase):
            def test_method(self):
                pass
            def tearDown(self):
                pass
        result = unittest.TestResult()
        DoesnotcalltearDown('test_method').run(result)
        self.assertThat(result.errors, HasLength(1))
        self.assertThat(result.errors[0][1],
            DocTestMatches(
                "...ValueError...File...testtools/tests/test_testcase.py...",
                ELLIPSIS))


class TestRunTwiceDeterminstic(TestCase):
    """Can we run the same test case twice?"""

    # XXX: Reviewer, please note that all of the other test cases in this
    # module are doing this wrong, saying 'run_test_with' instead of
    # 'run_tests_with'.
    run_tests_with = FullStackRunTest

    scenarios = deterministic_sample_cases_scenarios

    def test_runTwice(self):
        # Tests that are intrinsically determistic can be run twice and
        # produce exactly the same results each time, without need for
        # explicit resetting or reconstruction.
        test = make_case_for_behavior_scenario(self)
        first_result = ExtendedTestResult()
        test.run(first_result)
        second_result = ExtendedTestResult()
        test.run(second_result)
        self.assertEqual(first_result._events, second_result._events)


class TestRunTwiceNondeterministic(TestCase):
    """Can we run the same test case twice?

    Separate suite for non-deterministic tests, which require more complicated
    assertions and scenarios.
    """

    run_tests_with = FullStackRunTest

    scenarios = nondeterministic_sample_cases_scenarios

    def test_runTwice(self):
        test = self.case
        first_result = ExtendedTestResult()
        test.run(first_result)
        second_result = ExtendedTestResult()
        test.run(second_result)
        self.expectThat(
            first_result._events, self.expected_first_result)
        self.assertThat(
            second_result._events, self.expected_second_result)


class TestSkipping(TestCase):
    """Tests for skipping of tests functionality."""

    run_tests_with = FullStackRunTest

    def test_skip_causes_skipException(self):
        self.assertThat(
            lambda: self.skipTest("Skip this test"),
            Raises(MatchesException(self.skipException)))

    def test_can_use_skipTest(self):
        self.assertThat(
            lambda: self.skipTest("Skip this test"),
            Raises(MatchesException(self.skipException)))

    def test_skip_without_reason_works(self):
        class Test(TestCase):
            def test(self):
                raise self.skipException()
        case = Test("test")
        result = ExtendedTestResult()
        case.run(result)
        self.assertEqual('addSkip', result._events[1][0])
        self.assertEqual(
            'no reason given.',
            result._events[1][2]['reason'].as_text())

    def test_skipException_in_setup_calls_result_addSkip(self):
        class TestThatRaisesInSetUp(TestCase):
            def setUp(self):
                TestCase.setUp(self)
                self.skipTest("skipping this test")

            def test_that_passes(self):
                pass
        calls = []
        result = LoggingResult(calls)
        test = TestThatRaisesInSetUp("test_that_passes")
        test.run(result)
        case = result._events[0][1]
        self.assertEqual(
            [('startTest', case),
             ('addSkip', case, "skipping this test"),
             ('stopTest', case)],
            calls)

    def test_skipException_in_test_method_calls_result_addSkip(self):
        class SkippingTest(TestCase):
            def test_that_raises_skipException(self):
                self.skipTest("skipping this test")
        events = []
        result = Python27TestResult(events)
        test = SkippingTest("test_that_raises_skipException")
        test.run(result)
        case = result._events[0][1]
        self.assertEqual(
            [('startTest', case),
             ('addSkip', case, "skipping this test"),
             ('stopTest', case)],
            events)

    def test_different_skipException_in_test_method_calls_result_addSkip(self):
        class SkippingTest(TestCase):
            skipException = ValueError

            def test_that_raises_skipException(self):
                self.skipTest("skipping this test")

        events = []
        result = Python27TestResult(events)
        test = SkippingTest("test_that_raises_skipException")
        test.run(result)
        case = result._events[0][1]
        self.assertEqual(
            [('startTest', case),
             ('addSkip', case, "skipping this test"),
             ('stopTest', case)],
            events)

    def test_skip__in_setup_with_old_result_object_calls_addSuccess(self):

        class SkippingTest(TestCase):
            def setUp(self):
                TestCase.setUp(self)
                raise self.skipException("skipping this test")

            def test_that_raises_skipException(self):
                pass

        events = []
        result = Python26TestResult(events)
        test = SkippingTest("test_that_raises_skipException")
        test.run(result)
        self.assertEqual('addSuccess', events[1][0])

    def test_skip_with_old_result_object_calls_addError(self):
        class SkippingTest(TestCase):
            def test_that_raises_skipException(self):
                raise self.skipException("skipping this test")
        events = []
        result = Python26TestResult(events)
        test = SkippingTest("test_that_raises_skipException")
        test.run(result)
        self.assertEqual('addSuccess', events[1][0])

    def test_skip_decorator(self):
        class SkippingTest(TestCase):
            @skip("skipping this test")
            def test_that_is_decorated_with_skip(self):
                self.fail()
        events = []
        result = Python26TestResult(events)
        test = SkippingTest("test_that_is_decorated_with_skip")
        test.run(result)
        self.assertEqual('addSuccess', events[1][0])

    def test_skipIf_decorator(self):
        class SkippingTest(TestCase):
            @skipIf(True, "skipping this test")
            def test_that_is_decorated_with_skipIf(self):
                self.fail()
        events = []
        result = Python26TestResult(events)
        test = SkippingTest("test_that_is_decorated_with_skipIf")
        test.run(result)
        self.assertEqual('addSuccess', events[1][0])

    def test_skipUnless_decorator(self):
        class SkippingTest(TestCase):
            @skipUnless(False, "skipping this test")
            def test_that_is_decorated_with_skipUnless(self):
                self.fail()
        events = []
        result = Python26TestResult(events)
        test = SkippingTest("test_that_is_decorated_with_skipUnless")
        test.run(result)
        self.assertEqual('addSuccess', events[1][0])

    def test_skip_decorator_shared(self):
        def shared(testcase):
            testcase.fail('nope')

        class SkippingTest(TestCase):
            test_skip = skipIf(True, "skipping this test")(shared)

        class NotSkippingTest(TestCase):
            test_no_skip = skipIf(False, "skipping this test")(shared)

        events = []
        result = Python26TestResult(events)
        test = SkippingTest("test_skip")
        test.run(result)
        self.assertEqual('addSuccess', events[1][0])

        events2 = []
        result2 = Python26TestResult(events2)
        test2 = NotSkippingTest("test_no_skip")
        test2.run(result2)
        self.assertEqual('addFailure', events2[1][0])

    def test_skip_class_decorator(self):
        @skip("skipping this testcase")
        class SkippingTest(TestCase):
            def test_that_is_decorated_with_skip(self):
                self.fail()
        events = []
        result = Python26TestResult(events)
        try:
            test = SkippingTest("test_that_is_decorated_with_skip")
        except TestSkipped:
            self.fail('TestSkipped raised')
        test.run(result)
        self.assertEqual('addSuccess', events[1][0])

    def test_skipIf_class_decorator(self):
        @skipIf(True, "skipping this testcase")
        class SkippingTest(TestCase):
            def test_that_is_decorated_with_skipIf(self):
                self.fail()
        events = []
        result = Python26TestResult(events)
        try:
            test = SkippingTest("test_that_is_decorated_with_skipIf")
        except TestSkipped:
            self.fail('TestSkipped raised')
        test.run(result)
        self.assertEqual('addSuccess', events[1][0])

    def test_skipUnless_class_decorator(self):
        @skipUnless(False, "skipping this testcase")
        class SkippingTest(TestCase):
            def test_that_is_decorated_with_skipUnless(self):
                self.fail()
        events = []
        result = Python26TestResult(events)
        try:
            test = SkippingTest("test_that_is_decorated_with_skipUnless")
        except TestSkipped:
            self.fail('TestSkipped raised')
        test.run(result)
        self.assertEqual('addSuccess', events[1][0])

    def check_skip_decorator_does_not_run_setup(self, decorator, reason):
        class SkippingTest(TestCase):

            setup_ran = False

            def setUp(self):
                super().setUp()
                self.setup_ran = True

            # Use the decorator passed to us:
            @decorator
            def test_skipped(self):
                self.fail()

        test = SkippingTest('test_skipped')
        self.check_test_does_not_run_setup(test, reason)

        # Use the decorator passed to us:
        @decorator
        class SkippingTestCase(TestCase):

            setup_ran = False

            def setUp(self):
                super().setUp()
                self.setup_ran = True

            def test_skipped(self):
                self.fail()

        try:
            test = SkippingTestCase('test_skipped')
        except TestSkipped:
            self.fail('TestSkipped raised')
        self.check_test_does_not_run_setup(test, reason)

    def check_test_does_not_run_setup(self, test, reason):
        result = test.run()
        self.assertTrue(result.wasSuccessful())
        self.assertIn(reason, result.skip_reasons, result.skip_reasons)
        self.assertFalse(test.setup_ran)

    def test_testtools_skip_decorator_does_not_run_setUp(self):
        reason = self.getUniqueString()
        self.check_skip_decorator_does_not_run_setup(
            skip(reason),
            reason
        )

    def test_testtools_skipIf_decorator_does_not_run_setUp(self):
        reason = self.getUniqueString()
        self.check_skip_decorator_does_not_run_setup(
            skipIf(True, reason),
            reason
        )

    def test_testtools_skipUnless_decorator_does_not_run_setUp(self):
        reason = self.getUniqueString()
        self.check_skip_decorator_does_not_run_setup(
            skipUnless(False, reason),
            reason
        )

    def test_unittest_skip_decorator_does_not_run_setUp(self):
        reason = self.getUniqueString()
        self.check_skip_decorator_does_not_run_setup(
            unittest.skip(reason),
            reason
        )

    def test_unittest_skipIf_decorator_does_not_run_setUp(self):
        reason = self.getUniqueString()
        self.check_skip_decorator_does_not_run_setup(
            unittest.skipIf(True, reason),
            reason
        )

    def test_unittest_skipUnless_decorator_does_not_run_setUp(self):
        reason = self.getUniqueString()
        self.check_skip_decorator_does_not_run_setup(
            unittest.skipUnless(False, reason),
            reason
        )


class TestOnException(TestCase):

    run_test_with = FullStackRunTest

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

    run_tests_with = FullStackRunTest

    class Case(TestCase):
        def test(self):
            pass

    def run_test(self, test_body):
        """Run a test with ``test_body`` as the body.

        :return: Whatever ``test_body`` returns.
        """
        log = []
        def wrapper(case):
            log.append(test_body(case))
        case = make_test_case(self.getUniqueString(), test_body=wrapper)
        case.run()
        return log[0]

    def test_patch(self):
        # TestCase.patch masks obj.attribute with the new value.
        self.foo = 'original'
        def test_body(case):
            case.patch(self, 'foo', 'patched')
            return self.foo

        result = self.run_test(test_body)
        self.assertThat(result, Equals('patched'))

    def test_patch_restored_after_run(self):
        # TestCase.patch masks obj.attribute with the new value, but restores
        # the original value after the test is finished.
        self.foo = 'original'
        self.run_test(lambda case: case.patch(self, 'foo', 'patched'))
        self.assertThat(self.foo, Equals('original'))

    def test_successive_patches_apply(self):
        # TestCase.patch can be called multiple times per test. Each time you
        # call it, it overrides the original value.
        self.foo = 'original'
        def test_body(case):
            case.patch(self, 'foo', 'patched')
            case.patch(self, 'foo', 'second')
            return self.foo

        result = self.run_test(test_body)
        self.assertThat(result, Equals('second'))

    def test_successive_patches_restored_after_run(self):
        # TestCase.patch restores the original value, no matter how many times
        # it was called.
        self.foo = 'original'
        def test_body(case):
            case.patch(self, 'foo', 'patched')
            case.patch(self, 'foo', 'second')
            return self.foo

        self.run_test(test_body)
        self.assertThat(self.foo, Equals('original'))

    def test_patch_nonexistent_attribute(self):
        # TestCase.patch can be used to patch a non-existent attribute.
        def test_body(case):
            case.patch(self, 'doesntexist', 'patched')
            return self.doesntexist

        result = self.run_test(test_body)
        self.assertThat(result, Equals('patched'))

    def test_restore_nonexistent_attribute(self):
        # TestCase.patch can be used to patch a non-existent attribute, after
        # the test run, the attribute is then removed from the object.
        def test_body(case):
            case.patch(self, 'doesntexist', 'patched')
            return self.doesntexist

        self.run_test(test_body)
        marker = object()
        value = getattr(self, 'doesntexist', marker)
        self.assertIs(marker, value)


class TestTestCaseSuper(TestCase):

    run_test_with = FullStackRunTest

    def test_setup_uses_super(self):
        class OtherBaseCase(unittest.TestCase):
            setup_called = False
            def setUp(self):
                self.setup_called = True
                super().setUp()
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
                super().tearDown()
        class OurCase(TestCase, OtherBaseCase):
            def runTest(self):
                pass
        test = OurCase()
        test.setUp()
        test.tearDown()
        self.assertTrue(test.teardown_called)


class TestNullary(TestCase):

    def test_repr(self):
        # The repr() of nullary is the same as the repr() of the wrapped
        # function.
        def foo():
            pass
        wrapped = Nullary(foo)
        self.assertEqual(repr(wrapped), repr(foo))

    def test_called_with_arguments(self):
        # The function is called with the arguments given to Nullary's
        # constructor.
        l = []
        def foo(*args, **kwargs):
            l.append((args, kwargs))
        wrapped = Nullary(foo, 1, 2, a="b")
        wrapped()
        self.assertEqual(l, [((1, 2), {'a': 'b'})])

    def test_returns_wrapped(self):
        # Calling Nullary returns whatever the function returns.
        ret = object()
        wrapped = Nullary(lambda: ret)
        self.assertIs(ret, wrapped())

    def test_raises(self):
        # If the function raises, so does Nullary when called.
        wrapped = Nullary(lambda: 1/0)
        self.assertRaises(ZeroDivisionError, wrapped)


class Attributes(WithAttributes, TestCase):
    @attr('foo')
    def simple(self):
        pass

    # Not sorted here, forward or backwards.
    @attr('foo', 'quux', 'bar')
    def many(self):
        pass

    # Not sorted here, forward or backwards.
    @attr('bar')
    @attr('quux')
    @attr('foo')
    def decorated(self):
        pass


class TestAttributes(TestCase):

    def test_simple_attr(self):
        # Adding an attr to a test changes its id().
        case = Attributes('simple')
        self.assertEqual(
            'testtools.tests.test_testcase.Attributes.simple[foo]',
            case.id())

    def test_multiple_attributes(self):
        case = Attributes('many')
        self.assertEqual(
            'testtools.tests.test_testcase.Attributes.many[bar,foo,quux]',
            case.id())

    def test_multiple_attr_decorators(self):
        case = Attributes('decorated')
        self.assertEqual(
            'testtools.tests.test_testcase.Attributes.decorated[bar,foo,quux]',
            case.id())


class TestDecorateTestCaseResult(TestCase):

    def setUp(self):
        super().setUp()
        self.log = []

    def make_result(self, result):
        self.log.append(('result', result))
        return LoggingResult(self.log)

    def test___call__(self):
        case = DecorateTestCaseResult(PlaceHolder('foo'), self.make_result)
        case(None)
        case('something')
        self.assertEqual([('result', None),
            ('tags', set(), set()),
            ('startTest', case.decorated),
            ('addSuccess', case.decorated),
            ('stopTest', case.decorated),
            ('tags', set(), set()),
            ('result', 'something'),
            ('tags', set(), set()),
            ('startTest', case.decorated),
            ('addSuccess', case.decorated),
            ('stopTest', case.decorated),
            ('tags', set(), set())
            ], self.log)

    def test_run(self):
        case = DecorateTestCaseResult(PlaceHolder('foo'), self.make_result)
        case.run(None)
        case.run('something')
        self.assertEqual([('result', None),
            ('tags', set(), set()),
            ('startTest', case.decorated),
            ('addSuccess', case.decorated),
            ('stopTest', case.decorated),
            ('tags', set(), set()),
            ('result', 'something'),
            ('tags', set(), set()),
            ('startTest', case.decorated),
            ('addSuccess', case.decorated),
            ('stopTest', case.decorated),
            ('tags', set(), set())
            ], self.log)

    def test_before_after_hooks(self):
        case = DecorateTestCaseResult(PlaceHolder('foo'), self.make_result,
            before_run=lambda result: self.log.append('before'),
            after_run=lambda result: self.log.append('after'))
        case.run(None)
        case(None)
        self.assertEqual([
            ('result', None),
            'before',
            ('tags', set(), set()),
            ('startTest', case.decorated),
            ('addSuccess', case.decorated),
            ('stopTest', case.decorated),
            ('tags', set(), set()),
            'after',
            ('result', None),
            'before',
            ('tags', set(), set()),
            ('startTest', case.decorated),
            ('addSuccess', case.decorated),
            ('stopTest', case.decorated),
            ('tags', set(), set()),
            'after',
            ], self.log)

    def test_other_attribute(self):
        orig = PlaceHolder('foo')
        orig.thing = 'fred'
        case = DecorateTestCaseResult(orig, self.make_result)
        self.assertEqual('fred', case.thing)
        self.assertRaises(AttributeError, getattr, case, 'other')
        case.other = 'barbara'
        self.assertEqual('barbara', orig.other)
        del case.thing
        self.assertRaises(AttributeError, getattr, orig, 'thing')


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
