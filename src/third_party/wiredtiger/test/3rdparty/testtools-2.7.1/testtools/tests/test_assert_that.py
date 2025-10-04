from doctest import ELLIPSIS

from testtools import (
    TestCase,
    )
from testtools.assertions import (
    assert_that,
    )
from testtools.content import (
    TracebackContent,
    )
from testtools.matchers import (
    Annotate,
    DocTestMatches,
    Equals,
    )


class AssertThatTests:
    """A mixin containing shared tests for assertThat and assert_that."""

    def assert_that_callable(self, *args, **kwargs):
        raise NotImplementedError

    def assertFails(self, message, function, *args, **kwargs):
        """Assert that function raises a failure with the given message."""
        failure = self.assertRaises(
            self.failureException, function, *args, **kwargs)
        self.assert_that_callable(failure, DocTestMatches(message, ELLIPSIS))

    def test_assertThat_matches_clean(self):
        class Matcher:
            def match(self, foo):
                return None
        self.assert_that_callable("foo", Matcher())

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
        class Test(type(self)):
            def test(self):
                self.assert_that_callable("foo", Matcher())
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
        self.assertFails(expected, self.assert_that_callable, matchee, matcher)

    def test_assertThat_message_is_annotated(self):
        matchee = 'foo'
        matcher = Equals('bar')
        expected = Annotate('woo', matcher).match(matchee).describe()
        self.assertFails(expected,
                         self.assert_that_callable, matchee, matcher, 'woo')

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
            expected,
            self.assert_that_callable, matchee, matcher, verbose=True)

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
            self.failureException, self.assert_that_callable, matchee, matcher,
            verbose=True)
        self.assertEqual(expected, self.get_error_string(e))


class TestAssertThatFunction(AssertThatTests, TestCase):

    def assert_that_callable(self, *args, **kwargs):
        return assert_that(*args, **kwargs)


class TestAssertThatMethod(AssertThatTests, TestCase):

    def assert_that_callable(self, *args, **kwargs):
        return self.assertThat(*args, **kwargs)


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
