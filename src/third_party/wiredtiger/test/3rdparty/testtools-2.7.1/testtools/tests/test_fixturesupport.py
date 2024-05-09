# Copyright (c) 2010-2011 testtools developers. See LICENSE for details.

import unittest

from testtools import (
    TestCase,
    content,
    content_type,
    )
from testtools.compat import _b
from testtools.helpers import try_import
from testtools.matchers import (
    Contains,
    Equals,
    )
from testtools.testresult.doubles import (
    ExtendedTestResult,
    )

fixtures = try_import('fixtures')
LoggingFixture = try_import('fixtures.tests.helpers.LoggingFixture')


class TestFixtureSupport(TestCase):

    def setUp(self):
        super().setUp()
        if fixtures is None or LoggingFixture is None:
            self.skipTest("Need fixtures")

    def test_useFixture(self):
        fixture = LoggingFixture()
        class SimpleTest(TestCase):
            def test_foo(self):
                self.useFixture(fixture)
        result = unittest.TestResult()
        SimpleTest('test_foo').run(result)
        self.assertTrue(result.wasSuccessful())
        self.assertEqual(['setUp', 'cleanUp'], fixture.calls)

    def test_useFixture_cleanups_raise_caught(self):
        calls = []
        def raiser(ignored):
            calls.append('called')
            raise Exception('foo')
        fixture = fixtures.FunctionFixture(lambda:None, raiser)
        class SimpleTest(TestCase):
            def test_foo(self):
                self.useFixture(fixture)
        result = unittest.TestResult()
        SimpleTest('test_foo').run(result)
        self.assertFalse(result.wasSuccessful())
        self.assertEqual(['called'], calls)

    def test_useFixture_details_captured(self):
        class DetailsFixture(fixtures.Fixture):
            def setUp(self):
                fixtures.Fixture.setUp(self)
                self.addCleanup(delattr, self, 'content')
                self.content = [_b('content available until cleanUp')]
                self.addDetail('content',
                    content.Content(content_type.UTF8_TEXT, self.get_content))
            def get_content(self):
                return self.content
        fixture = DetailsFixture()
        class SimpleTest(TestCase):
            def test_foo(self):
                self.useFixture(fixture)
                # Add a colliding detail (both should show up)
                self.addDetail('content',
                    content.Content(content_type.UTF8_TEXT, lambda:[_b('foo')]))
        result = ExtendedTestResult()
        SimpleTest('test_foo').run(result)
        self.assertEqual('addSuccess', result._events[-2][0])
        details = result._events[-2][2]
        self.assertEqual(['content', 'content-1'], sorted(details.keys()))
        self.assertEqual('foo', details['content'].as_text())
        self.assertEqual('content available until cleanUp',
            details['content-1'].as_text())

    def test_useFixture_multiple_details_captured(self):
        class DetailsFixture(fixtures.Fixture):
            def setUp(self):
                fixtures.Fixture.setUp(self)
                self.addDetail('aaa', content.text_content("foo"))
                self.addDetail('bbb', content.text_content("bar"))
        fixture = DetailsFixture()
        class SimpleTest(TestCase):
            def test_foo(self):
                self.useFixture(fixture)
        result = ExtendedTestResult()
        SimpleTest('test_foo').run(result)
        self.assertEqual('addSuccess', result._events[-2][0])
        details = result._events[-2][2]
        self.assertEqual(['aaa', 'bbb'], sorted(details))
        self.assertEqual('foo', details['aaa'].as_text())
        self.assertEqual('bar', details['bbb'].as_text())

    def test_useFixture_details_captured_from_setUp(self):
        # Details added during fixture set-up are gathered even if setUp()
        # fails with an exception.
        class BrokenFixture(fixtures.Fixture):
            def setUp(self):
                fixtures.Fixture.setUp(self)
                self.addDetail('content', content.text_content("foobar"))
                raise Exception()
        fixture = BrokenFixture()
        class SimpleTest(TestCase):
            def test_foo(self):
                self.useFixture(fixture)
        result = ExtendedTestResult()
        SimpleTest('test_foo').run(result)
        self.assertEqual('addError', result._events[-2][0])
        details = result._events[-2][2]
        self.assertEqual(['content', 'traceback'], sorted(details))
        self.assertEqual('foobar', ''.join(details['content'].iter_text()))

    def test_useFixture_details_captured_from__setUp(self):
        # Newer Fixtures deprecates setUp() in favour of _setUp().
        # https://bugs.launchpad.net/testtools/+bug/1469759 reports that
        # this is broken when gathering details from a broken _setUp().
        class BrokenFixture(fixtures.Fixture):
            def _setUp(self):
                fixtures.Fixture._setUp(self)
                self.addDetail('broken', content.text_content("foobar"))
                raise Exception("_setUp broke")
        fixture = BrokenFixture()
        class SimpleTest(TestCase):
            def test_foo(self):
                self.addDetail('foo_content', content.text_content("foo ok"))
                self.useFixture(fixture)
        result = ExtendedTestResult()
        SimpleTest('test_foo').run(result)
        self.assertEqual('addError', result._events[-2][0])
        details = result._events[-2][2]
        self.assertEqual(
            ['broken', 'foo_content', 'traceback', 'traceback-1'],
            sorted(details))
        self.expectThat(
            ''.join(details['broken'].iter_text()),
            Equals('foobar'))
        self.expectThat(
            ''.join(details['foo_content'].iter_text()),
            Equals('foo ok'))
        self.expectThat(
            ''.join(details['traceback'].iter_text()),
            Contains('_setUp broke'))
        self.expectThat(
            ''.join(details['traceback-1'].iter_text()),
            Contains('foobar'))

    def test_useFixture_original_exception_raised_if_gather_details_fails(self):
        # In bug #1368440 it was reported that when a fixture fails setUp
        # and gather_details errors on it, then the original exception that
        # failed is not reported.
        class BrokenFixture(fixtures.Fixture):
            def getDetails(self):
                raise AttributeError("getDetails broke")
            def setUp(self):
                fixtures.Fixture.setUp(self)
                raise Exception("setUp broke")
        fixture = BrokenFixture()
        class SimpleTest(TestCase):
            def test_foo(self):
                self.useFixture(fixture)
        result = ExtendedTestResult()
        SimpleTest('test_foo').run(result)
        self.assertEqual('addError', result._events[-2][0])
        details = result._events[-2][2]
        self.assertEqual(['traceback', 'traceback-1'], sorted(details))
        self.assertThat(
            ''.join(details['traceback'].iter_text()),
            Contains('setUp broke'))
        self.assertThat(
            ''.join(details['traceback-1'].iter_text()),
            Contains('getDetails broke'))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
