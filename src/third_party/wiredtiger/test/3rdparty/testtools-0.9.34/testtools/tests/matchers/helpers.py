# Copyright (c) 2008-2012 testtools developers. See LICENSE for details.

from testtools.tests.helpers import FullStackRunTest


class TestMatchersInterface(object):

    run_tests_with = FullStackRunTest

    def test_matches_match(self):
        matcher = self.matches_matcher
        matches = self.matches_matches
        mismatches = self.matches_mismatches
        for candidate in matches:
            self.assertEqual(None, matcher.match(candidate))
        for candidate in mismatches:
            mismatch = matcher.match(candidate)
            self.assertNotEqual(None, mismatch)
            self.assertNotEqual(None, getattr(mismatch, 'describe', None))

    def test__str__(self):
        # [(expected, object to __str__)].
        from testtools.matchers._doctest import DocTestMatches
        examples = self.str_examples
        for expected, matcher in examples:
            self.assertThat(matcher, DocTestMatches(expected))

    def test_describe_difference(self):
        # [(expected, matchee, matcher), ...]
        examples = self.describe_examples
        for difference, matchee, matcher in examples:
            mismatch = matcher.match(matchee)
            self.assertEqual(difference, mismatch.describe())

    def test_mismatch_details(self):
        # The mismatch object must provide get_details, which must return a
        # dictionary mapping names to Content objects.
        examples = self.describe_examples
        for difference, matchee, matcher in examples:
            mismatch = matcher.match(matchee)
            details = mismatch.get_details()
            self.assertEqual(dict(details), details)
