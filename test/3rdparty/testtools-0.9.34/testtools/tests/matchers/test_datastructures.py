# Copyright (c) 2008-2012 testtools developers. See LICENSE for details.

import doctest
import re
import sys

from testtools import TestCase
from testtools.compat import StringIO
from testtools.matchers import (
    Annotate,
    Equals,
    LessThan,
    MatchesRegex,
    NotEquals,
    )
from testtools.matchers._datastructures import (
    ContainsAll,
    MatchesListwise,
    MatchesStructure,
    MatchesSetwise,
    )
from testtools.tests.helpers import FullStackRunTest
from testtools.tests.matchers.helpers import TestMatchersInterface


def run_doctest(obj, name):
    p = doctest.DocTestParser()
    t = p.get_doctest(
        obj.__doc__, sys.modules[obj.__module__].__dict__, name, '', 0)
    r = doctest.DocTestRunner()
    output = StringIO()
    r.run(t, out=output.write)
    return r.failures, output.getvalue()


class TestMatchesListwise(TestCase):

    run_tests_with = FullStackRunTest

    def test_docstring(self):
        failure_count, output = run_doctest(
            MatchesListwise, "MatchesListwise")
        if failure_count:
            self.fail("Doctest failed with %s" % output)


class TestMatchesStructure(TestCase, TestMatchersInterface):

    class SimpleClass:
        def __init__(self, x, y):
            self.x = x
            self.y = y

    matches_matcher = MatchesStructure(x=Equals(1), y=Equals(2))
    matches_matches = [SimpleClass(1, 2)]
    matches_mismatches = [
        SimpleClass(2, 2),
        SimpleClass(1, 1),
        SimpleClass(3, 3),
        ]

    str_examples = [
        ("MatchesStructure(x=Equals(1))", MatchesStructure(x=Equals(1))),
        ("MatchesStructure(y=Equals(2))", MatchesStructure(y=Equals(2))),
        ("MatchesStructure(x=Equals(1), y=Equals(2))",
         MatchesStructure(x=Equals(1), y=Equals(2))),
        ]

    describe_examples = [
        ("""\
Differences: [
3 != 1: x
]""", SimpleClass(1, 2), MatchesStructure(x=Equals(3), y=Equals(2))),
        ("""\
Differences: [
3 != 2: y
]""", SimpleClass(1, 2), MatchesStructure(x=Equals(1), y=Equals(3))),
        ("""\
Differences: [
0 != 1: x
0 != 2: y
]""", SimpleClass(1, 2), MatchesStructure(x=Equals(0), y=Equals(0))),
        ]

    def test_fromExample(self):
        self.assertThat(
            self.SimpleClass(1, 2),
            MatchesStructure.fromExample(self.SimpleClass(1, 3), 'x'))

    def test_byEquality(self):
        self.assertThat(
            self.SimpleClass(1, 2),
            MatchesStructure.byEquality(x=1))

    def test_withStructure(self):
        self.assertThat(
            self.SimpleClass(1, 2),
            MatchesStructure.byMatcher(LessThan, x=2))

    def test_update(self):
        self.assertThat(
            self.SimpleClass(1, 2),
            MatchesStructure(x=NotEquals(1)).update(x=Equals(1)))

    def test_update_none(self):
        self.assertThat(
            self.SimpleClass(1, 2),
            MatchesStructure(x=Equals(1), z=NotEquals(42)).update(
                z=None))


class TestMatchesSetwise(TestCase):

    run_tests_with = FullStackRunTest

    def assertMismatchWithDescriptionMatching(self, value, matcher,
                                              description_matcher):
        mismatch = matcher.match(value)
        if mismatch is None:
            self.fail("%s matched %s" % (matcher, value))
        actual_description = mismatch.describe()
        self.assertThat(
            actual_description,
            Annotate(
                "%s matching %s" % (matcher, value),
                description_matcher))

    def test_matches(self):
        self.assertIs(
            None, MatchesSetwise(Equals(1), Equals(2)).match([2, 1]))

    def test_mismatches(self):
        self.assertMismatchWithDescriptionMatching(
            [2, 3], MatchesSetwise(Equals(1), Equals(2)),
            MatchesRegex('.*There was 1 mismatch$', re.S))

    def test_too_many_matchers(self):
        self.assertMismatchWithDescriptionMatching(
            [2, 3], MatchesSetwise(Equals(1), Equals(2), Equals(3)),
            Equals('There was 1 matcher left over: Equals(1)'))

    def test_too_many_values(self):
        self.assertMismatchWithDescriptionMatching(
            [1, 2, 3], MatchesSetwise(Equals(1), Equals(2)),
            Equals('There was 1 value left over: [3]'))

    def test_two_too_many_matchers(self):
        self.assertMismatchWithDescriptionMatching(
            [3], MatchesSetwise(Equals(1), Equals(2), Equals(3)),
            MatchesRegex(
                'There were 2 matchers left over: Equals\([12]\), '
                'Equals\([12]\)'))

    def test_two_too_many_values(self):
        self.assertMismatchWithDescriptionMatching(
            [1, 2, 3, 4], MatchesSetwise(Equals(1), Equals(2)),
            MatchesRegex(
                'There were 2 values left over: \[[34], [34]\]'))

    def test_mismatch_and_too_many_matchers(self):
        self.assertMismatchWithDescriptionMatching(
            [2, 3], MatchesSetwise(Equals(0), Equals(1), Equals(2)),
            MatchesRegex(
                '.*There was 1 mismatch and 1 extra matcher: Equals\([01]\)',
                re.S))

    def test_mismatch_and_too_many_values(self):
        self.assertMismatchWithDescriptionMatching(
            [2, 3, 4], MatchesSetwise(Equals(1), Equals(2)),
            MatchesRegex(
                '.*There was 1 mismatch and 1 extra value: \[[34]\]',
                re.S))

    def test_mismatch_and_two_too_many_matchers(self):
        self.assertMismatchWithDescriptionMatching(
            [3, 4], MatchesSetwise(
                Equals(0), Equals(1), Equals(2), Equals(3)),
            MatchesRegex(
                '.*There was 1 mismatch and 2 extra matchers: '
                'Equals\([012]\), Equals\([012]\)', re.S))

    def test_mismatch_and_two_too_many_values(self):
        self.assertMismatchWithDescriptionMatching(
            [2, 3, 4, 5], MatchesSetwise(Equals(1), Equals(2)),
            MatchesRegex(
                '.*There was 1 mismatch and 2 extra values: \[[145], [145]\]',
                re.S))


class TestContainsAllInterface(TestCase, TestMatchersInterface):

    matches_matcher = ContainsAll(['foo', 'bar'])
    matches_matches = [['foo', 'bar'], ['foo', 'z', 'bar'], ['bar', 'foo']]
    matches_mismatches = [['f', 'g'], ['foo', 'baz'], []]

    str_examples = [(
        "MatchesAll(Contains('foo'), Contains('bar'))",
        ContainsAll(['foo', 'bar'])),
        ]

    describe_examples = [("""Differences: [
'baz' not in 'foo'
]""",
    'foo', ContainsAll(['foo', 'baz']))]


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
