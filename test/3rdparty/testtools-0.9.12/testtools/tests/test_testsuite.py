# Copyright (c) 2009-2011 testtools developers. See LICENSE for details.

"""Test ConcurrentTestSuite and related things."""

__metaclass__ = type

import unittest

from testtools import (
    ConcurrentTestSuite,
    iterate_tests,
    TestCase,
    )
from testtools.helpers import try_import
from testtools.testsuite import FixtureSuite
from testtools.tests.helpers import LoggingResult

FunctionFixture = try_import('fixtures.FunctionFixture')


class TestConcurrentTestSuiteRun(TestCase):

    def test_trivial(self):
        log = []
        result = LoggingResult(log)
        class Sample(TestCase):
            def __hash__(self):
                return id(self)

            def test_method1(self):
                pass
            def test_method2(self):
                pass
        test1 = Sample('test_method1')
        test2 = Sample('test_method2')
        original_suite = unittest.TestSuite([test1, test2])
        suite = ConcurrentTestSuite(original_suite, self.split_suite)
        suite.run(result)
        # 0 is the timestamp for the first test starting.
        test1 = log[1][1]
        test2 = log[-1][1]
        self.assertIsInstance(test1, Sample)
        self.assertIsInstance(test2, Sample)
        self.assertNotEqual(test1.id(), test2.id())

    def split_suite(self, suite):
        tests = list(iterate_tests(suite))
        return tests[0], tests[1]


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


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
