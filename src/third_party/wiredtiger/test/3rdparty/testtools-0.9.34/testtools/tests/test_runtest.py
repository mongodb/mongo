# Copyright (c) 2009-2011 testtools developers. See LICENSE for details.

"""Tests for the RunTest single test execution logic."""

from testtools import (
    ExtendedToOriginalDecorator,
    run_test_with,
    RunTest,
    TestCase,
    TestResult,
    )
from testtools.matchers import MatchesException, Is, Raises
from testtools.testresult.doubles import ExtendedTestResult
from testtools.tests.helpers import FullStackRunTest


class TestRunTest(TestCase):

    run_tests_with = FullStackRunTest

    def make_case(self):
        class Case(TestCase):
            def test(self):
                pass
        return Case('test')

    def test___init___short(self):
        run = RunTest("bar")
        self.assertEqual("bar", run.case)
        self.assertEqual([], run.handlers)

    def test__init____handlers(self):
        handlers = [("quux", "baz")]
        run = RunTest("bar", handlers)
        self.assertEqual(handlers, run.handlers)

    def test_run_with_result(self):
        # test.run passes result down to _run_test_method.
        log = []
        class Case(TestCase):
            def _run_test_method(self, result):
                log.append(result)
        case = Case('_run_test_method')
        run = RunTest(case, lambda x: log.append(x))
        result = TestResult()
        run.run(result)
        self.assertEqual(1, len(log))
        self.assertEqual(result, log[0].decorated)

    def test_run_no_result_manages_new_result(self):
        log = []
        run = RunTest(self.make_case(), lambda x: log.append(x) or x)
        result = run.run()
        self.assertIsInstance(result.decorated, TestResult)

    def test__run_core_called(self):
        case = self.make_case()
        log = []
        run = RunTest(case, lambda x: x)
        run._run_core = lambda: log.append('foo')
        run.run()
        self.assertEqual(['foo'], log)

    def test__run_user_does_not_catch_keyboard(self):
        case = self.make_case()
        def raises():
            raise KeyboardInterrupt("yo")
        run = RunTest(case, None)
        run.result = ExtendedTestResult()
        self.assertThat(lambda: run._run_user(raises),
            Raises(MatchesException(KeyboardInterrupt)))
        self.assertEqual([], run.result._events)

    def test__run_user_calls_onException(self):
        case = self.make_case()
        log = []
        def handler(exc_info):
            log.append("got it")
            self.assertEqual(3, len(exc_info))
            self.assertIsInstance(exc_info[1], KeyError)
            self.assertIs(KeyError, exc_info[0])
        case.addOnException(handler)
        e = KeyError('Yo')
        def raises():
            raise e
        run = RunTest(case, [(KeyError, None)])
        run.result = ExtendedTestResult()
        status = run._run_user(raises)
        self.assertEqual(run.exception_caught, status)
        self.assertEqual([], run.result._events)
        self.assertEqual(["got it"], log)

    def test__run_user_can_catch_Exception(self):
        case = self.make_case()
        e = Exception('Yo')
        def raises():
            raise e
        log = []
        run = RunTest(case, [(Exception, None)])
        run.result = ExtendedTestResult()
        status = run._run_user(raises)
        self.assertEqual(run.exception_caught, status)
        self.assertEqual([], run.result._events)
        self.assertEqual([], log)

    def test__run_user_uncaught_Exception_raised(self):
        case = self.make_case()
        e = KeyError('Yo')
        def raises():
            raise e
        log = []
        def log_exc(self, result, err):
            log.append((result, err))
        run = RunTest(case, [(ValueError, log_exc)])
        run.result = ExtendedTestResult()
        self.assertThat(lambda: run._run_user(raises),
            Raises(MatchesException(KeyError)))
        self.assertEqual([], run.result._events)
        self.assertEqual([], log)

    def test__run_user_uncaught_Exception_from_exception_handler_raised(self):
        case = self.make_case()
        def broken_handler(exc_info):
            # ValueError because thats what we know how to catch - and must
            # not.
            raise ValueError('boo')
        case.addOnException(broken_handler)
        e = KeyError('Yo')
        def raises():
            raise e
        log = []
        def log_exc(self, result, err):
            log.append((result, err))
        run = RunTest(case, [(ValueError, log_exc)])
        run.result = ExtendedTestResult()
        self.assertThat(lambda: run._run_user(raises),
            Raises(MatchesException(ValueError)))
        self.assertEqual([], run.result._events)
        self.assertEqual([], log)

    def test__run_user_returns_result(self):
        case = self.make_case()
        def returns():
            return 1
        run = RunTest(case)
        run.result = ExtendedTestResult()
        self.assertEqual(1, run._run_user(returns))
        self.assertEqual([], run.result._events)

    def test__run_one_decorates_result(self):
        log = []
        class Run(RunTest):
            def _run_prepared_result(self, result):
                log.append(result)
                return result
        run = Run(self.make_case(), lambda x: x)
        result = run._run_one('foo')
        self.assertEqual([result], log)
        self.assertIsInstance(log[0], ExtendedToOriginalDecorator)
        self.assertEqual('foo', result.decorated)

    def test__run_prepared_result_calls_start_and_stop_test(self):
        result = ExtendedTestResult()
        case = self.make_case()
        run = RunTest(case, lambda x: x)
        run.run(result)
        self.assertEqual([
            ('startTest', case),
            ('addSuccess', case),
            ('stopTest', case),
            ], result._events)

    def test__run_prepared_result_calls_stop_test_always(self):
        result = ExtendedTestResult()
        case = self.make_case()
        def inner():
            raise Exception("foo")
        run = RunTest(case, lambda x: x)
        run._run_core = inner
        self.assertThat(lambda: run.run(result),
            Raises(MatchesException(Exception("foo"))))
        self.assertEqual([
            ('startTest', case),
            ('stopTest', case),
            ], result._events)


class CustomRunTest(RunTest):

    marker = object()

    def run(self, result=None):
        return self.marker


class TestTestCaseSupportForRunTest(TestCase):

    def test_pass_custom_run_test(self):
        class SomeCase(TestCase):
            def test_foo(self):
                pass
        result = TestResult()
        case = SomeCase('test_foo', runTest=CustomRunTest)
        from_run_test = case.run(result)
        self.assertThat(from_run_test, Is(CustomRunTest.marker))

    def test_default_is_runTest_class_variable(self):
        class SomeCase(TestCase):
            run_tests_with = CustomRunTest
            def test_foo(self):
                pass
        result = TestResult()
        case = SomeCase('test_foo')
        from_run_test = case.run(result)
        self.assertThat(from_run_test, Is(CustomRunTest.marker))

    def test_constructor_argument_overrides_class_variable(self):
        # If a 'runTest' argument is passed to the test's constructor, that
        # overrides the class variable.
        marker = object()
        class DifferentRunTest(RunTest):
            def run(self, result=None):
                return marker
        class SomeCase(TestCase):
            run_tests_with = CustomRunTest
            def test_foo(self):
                pass
        result = TestResult()
        case = SomeCase('test_foo', runTest=DifferentRunTest)
        from_run_test = case.run(result)
        self.assertThat(from_run_test, Is(marker))

    def test_decorator_for_run_test(self):
        # Individual test methods can be marked as needing a special runner.
        class SomeCase(TestCase):
            @run_test_with(CustomRunTest)
            def test_foo(self):
                pass
        result = TestResult()
        case = SomeCase('test_foo')
        from_run_test = case.run(result)
        self.assertThat(from_run_test, Is(CustomRunTest.marker))

    def test_extended_decorator_for_run_test(self):
        # Individual test methods can be marked as needing a special runner.
        # Extra arguments can be passed to the decorator which will then be
        # passed on to the RunTest object.
        marker = object()
        class FooRunTest(RunTest):
            def __init__(self, case, handlers=None, bar=None):
                super(FooRunTest, self).__init__(case, handlers)
                self.bar = bar
            def run(self, result=None):
                return self.bar
        class SomeCase(TestCase):
            @run_test_with(FooRunTest, bar=marker)
            def test_foo(self):
                pass
        result = TestResult()
        case = SomeCase('test_foo')
        from_run_test = case.run(result)
        self.assertThat(from_run_test, Is(marker))

    def test_works_as_inner_decorator(self):
        # Even if run_test_with is the innermost decorator, it will be
        # respected.
        def wrapped(function):
            """Silly, trivial decorator."""
            def decorated(*args, **kwargs):
                return function(*args, **kwargs)
            decorated.__name__ = function.__name__
            decorated.__dict__.update(function.__dict__)
            return decorated
        class SomeCase(TestCase):
            @wrapped
            @run_test_with(CustomRunTest)
            def test_foo(self):
                pass
        result = TestResult()
        case = SomeCase('test_foo')
        from_run_test = case.run(result)
        self.assertThat(from_run_test, Is(CustomRunTest.marker))

    def test_constructor_overrides_decorator(self):
        # If a 'runTest' argument is passed to the test's constructor, that
        # overrides the decorator.
        marker = object()
        class DifferentRunTest(RunTest):
            def run(self, result=None):
                return marker
        class SomeCase(TestCase):
            @run_test_with(CustomRunTest)
            def test_foo(self):
                pass
        result = TestResult()
        case = SomeCase('test_foo', runTest=DifferentRunTest)
        from_run_test = case.run(result)
        self.assertThat(from_run_test, Is(marker))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
