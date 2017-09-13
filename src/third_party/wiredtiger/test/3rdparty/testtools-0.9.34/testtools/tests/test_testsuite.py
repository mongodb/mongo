# Copyright (c) 2009-2011 testtools developers. See LICENSE for details.

"""Test ConcurrentTestSuite and related things."""

__metaclass__ = type

import doctest
from functools import partial
import sys
import unittest

from extras import try_import

from testtools import (
    ConcurrentTestSuite,
    ConcurrentStreamTestSuite,
    iterate_tests,
    PlaceHolder,
    TestByTestResult,
    TestCase,
    )
from testtools.compat import _b, _u
from testtools.matchers import DocTestMatches
from testtools.testsuite import FixtureSuite, iterate_tests, sorted_tests
from testtools.tests.helpers import LoggingResult
from testtools.testresult.doubles import StreamResult as LoggingStream

FunctionFixture = try_import('fixtures.FunctionFixture')

class Sample(TestCase):
    def __hash__(self):
        return id(self)
    def test_method1(self):
        pass
    def test_method2(self):
        pass


class TestConcurrentTestSuiteRun(TestCase):

    def test_broken_test(self):
        log = []
        def on_test(test, status, start_time, stop_time, tags, details):
            log.append((test.id(), status, set(details.keys())))
        class BrokenTest(object):
            # Simple break - no result parameter to run()
            def __call__(self):
                pass
            run = __call__
        original_suite = unittest.TestSuite([BrokenTest()])
        suite = ConcurrentTestSuite(original_suite, self.split_suite)
        suite.run(TestByTestResult(on_test))
        self.assertEqual([('broken-runner', 'error', set(['traceback']))], log)

    def test_trivial(self):
        log = []
        result = LoggingResult(log)
        test1 = Sample('test_method1')
        test2 = Sample('test_method2')
        original_suite = unittest.TestSuite([test1, test2])
        suite = ConcurrentTestSuite(original_suite, self.split_suite)
        suite.run(result)
        # log[0] is the timestamp for the first test starting.
        test1 = log[1][1]
        test2 = log[-1][1]
        self.assertIsInstance(test1, Sample)
        self.assertIsInstance(test2, Sample)
        self.assertNotEqual(test1.id(), test2.id())

    def test_wrap_result(self):
        # ConcurrentTestSuite has a hook for wrapping the per-thread result.
        wrap_log = []

        def wrap_result(thread_safe_result, thread_number):
            wrap_log.append(
                (thread_safe_result.result.decorated, thread_number))
            return thread_safe_result

        result_log = []
        result = LoggingResult(result_log)
        test1 = Sample('test_method1')
        test2 = Sample('test_method2')
        original_suite = unittest.TestSuite([test1, test2])
        suite = ConcurrentTestSuite(
            original_suite, self.split_suite, wrap_result=wrap_result)
        suite.run(result)
        self.assertEqual(
            [(result, 0),
             (result, 1),
             ], wrap_log)
        # Smoke test to make sure everything ran OK.
        self.assertNotEqual([], result_log)

    def split_suite(self, suite):
        return list(iterate_tests(suite))


class TestConcurrentStreamTestSuiteRun(TestCase):

    def test_trivial(self):
        result = LoggingStream()
        test1 = Sample('test_method1')
        test2 = Sample('test_method2')
        cases = lambda:[(test1, '0'), (test2, '1')]
        suite = ConcurrentStreamTestSuite(cases)
        suite.run(result)
        def freeze(set_or_none):
            if set_or_none is None:
                return set_or_none
            return frozenset(set_or_none)
        # Ignore event order: we're testing the code is all glued together,
        # which just means we can pump events through and they get route codes
        # added appropriately.
        self.assertEqual(set([
            ('status',
             'testtools.tests.test_testsuite.Sample.test_method1',
             'inprogress',
             None,
             True,
             None,
             None,
             False,
             None,
             '0',
             None,
             ),
            ('status',
             'testtools.tests.test_testsuite.Sample.test_method1',
             'success',
             frozenset(),
             True,
             None,
             None,
             False,
             None,
             '0',
             None,
             ),
            ('status',
             'testtools.tests.test_testsuite.Sample.test_method2',
             'inprogress',
             None,
             True,
             None,
             None,
             False,
             None,
             '1',
             None,
             ),
            ('status',
             'testtools.tests.test_testsuite.Sample.test_method2',
             'success',
             frozenset(),
             True,
             None,
             None,
             False,
             None,
             '1',
             None,
             ),
            ]), set(event[0:3] + (freeze(event[3]),) + event[4:10] + (None,)
                for event in result._events))

    def test_broken_runner(self):
        # If the object called breaks, the stream is informed about it
        # regardless.
        class BrokenTest(object):
            # broken - no result parameter!
            def __call__(self):
                pass
            def run(self):
                pass
        result = LoggingStream()
        cases = lambda:[(BrokenTest(), '0')]
        suite = ConcurrentStreamTestSuite(cases)
        suite.run(result)
        events = result._events
        # Check the traceback loosely.
        self.assertThat(events[1][6].decode('utf8'), DocTestMatches("""\
Traceback (most recent call last):
  File "...testtools/testsuite.py", line ..., in _run_test
    test.run(process_result)
TypeError: run() takes ...1 ...argument...2...given...
""", doctest.ELLIPSIS))
        events = [event[0:10] + (None,) for event in events]
        events[1] = events[1][:6] + (None,) + events[1][7:]
        self.assertEqual([
            ('status', "broken-runner-'0'", 'inprogress', None, True, None, None, False, None, _u('0'), None),
            ('status', "broken-runner-'0'", None, None, True, 'traceback', None,
             False,
             'text/x-traceback; charset="utf8"; language="python"',
             '0',
             None),
             ('status', "broken-runner-'0'", None, None, True, 'traceback', b'', True,
              'text/x-traceback; charset="utf8"; language="python"', '0', None),
             ('status', "broken-runner-'0'", 'fail', set(), True, None, None, False, None, _u('0'), None)
            ], events)

    def split_suite(self, suite):
        tests = list(enumerate(iterate_tests(suite)))
        return [(test, _u(str(pos))) for pos, test in tests]


class TestFixtureSuite(TestCase):

    def setUp(self):
        super(TestFixtureSuite, self).setUp()
        if FunctionFixture is None:
            self.skip("Need fixtures")

    def test_fixture_suite(self):
        log = []
        class Sample(TestCase):
            def test_one(self):
                log.append(1)
            def test_two(self):
                log.append(2)
        fixture = FunctionFixture(
            lambda: log.append('setUp'),
            lambda fixture: log.append('tearDown'))
        suite = FixtureSuite(fixture, [Sample('test_one'), Sample('test_two')])
        suite.run(LoggingResult([]))
        self.assertEqual(['setUp', 1, 2, 'tearDown'], log)

    def test_fixture_suite_sort(self):
        log = []
        class Sample(TestCase):
            def test_one(self):
                log.append(1)
            def test_two(self):
                log.append(2)
        fixture = FunctionFixture(
            lambda: log.append('setUp'),
            lambda fixture: log.append('tearDown'))
        suite = FixtureSuite(fixture, [Sample('test_one'), Sample('test_one')])
        self.assertRaises(ValueError, suite.sort_tests)


class TestSortedTests(TestCase):

    def test_sorts_custom_suites(self):
        a = PlaceHolder('a')
        b = PlaceHolder('b')
        class Subclass(unittest.TestSuite):
            def sort_tests(self):
                self._tests = sorted_tests(self, True)
        input_suite = Subclass([b, a])
        suite = sorted_tests(input_suite)
        self.assertEqual([a, b], list(iterate_tests(suite)))
        self.assertEqual([input_suite], list(iter(suite)))

    def test_custom_suite_without_sort_tests_works(self):
        a = PlaceHolder('a')
        b = PlaceHolder('b')
        class Subclass(unittest.TestSuite):pass
        input_suite = Subclass([b, a])
        suite = sorted_tests(input_suite)
        self.assertEqual([b, a], list(iterate_tests(suite)))
        self.assertEqual([input_suite], list(iter(suite)))

    def test_sorts_simple_suites(self):
        a = PlaceHolder('a')
        b = PlaceHolder('b')
        suite = sorted_tests(unittest.TestSuite([b, a]))
        self.assertEqual([a, b], list(iterate_tests(suite)))

    def test_duplicate_simple_suites(self):
        a = PlaceHolder('a')
        b = PlaceHolder('b')
        c = PlaceHolder('a')
        self.assertRaises(
            ValueError, sorted_tests, unittest.TestSuite([a, b, c]))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
