# Copyright (c) 2010-2011 testtools developers. See LICENSE for details.

import unittest

from testtools import (
    TestCase,
    content,
    content_type,
    )
from testtools.compat import _b, _u
from testtools.helpers import try_import
from testtools.testresult.doubles import (
    ExtendedTestResult,
    )

fixtures = try_import('fixtures')
LoggingFixture = try_import('fixtures.tests.helpers.LoggingFixture')


class TestFixtureSupport(TestCase):

    def setUp(self):
        super(TestFixtureSupport, self).setUp()
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
        self.assertEqual('foo', _u('').join(details['content'].iter_text()))
        self.assertEqual('content available until cleanUp',
            ''.join(details['content-1'].iter_text()))

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
        self.assertEqual('foo', ''.join(details['aaa'].iter_text()))
        self.assertEqual('bar', ''.join(details['bbb'].iter_text()))

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


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
