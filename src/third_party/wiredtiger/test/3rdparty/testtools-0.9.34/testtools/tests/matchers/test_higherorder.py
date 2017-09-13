# Copyright (c) 2008-2011 testtools developers. See LICENSE for details.

from testtools import TestCase
from testtools.matchers import (
    DocTestMatches,
    Equals,
    LessThan,
    MatchesStructure,
    Mismatch,
    NotEquals,
    )
from testtools.matchers._higherorder import (
    AfterPreprocessing,
    AllMatch,
    Annotate,
    AnnotatedMismatch,
    AnyMatch,
    MatchesAny,
    MatchesAll,
    MatchesPredicate,
    MatchesPredicateWithParams,
    Not,
    )
from testtools.tests.helpers import FullStackRunTest
from testtools.tests.matchers.helpers import TestMatchersInterface


class TestAllMatch(TestCase, TestMatchersInterface):

    matches_matcher = AllMatch(LessThan(10))
    matches_matches = [
        [9, 9, 9],
        (9, 9),
        iter([9, 9, 9, 9, 9]),
        ]
    matches_mismatches = [
        [11, 9, 9],
        iter([9, 12, 9, 11]),
        ]

    str_examples = [
        ("AllMatch(LessThan(12))", AllMatch(LessThan(12))),
        ]

    describe_examples = [
        ('Differences: [\n'
         '10 is not > 11\n'
         '10 is not > 10\n'
         ']',
         [11, 9, 10],
         AllMatch(LessThan(10))),
        ]


class TestAnyMatch(TestCase, TestMatchersInterface):

    matches_matcher = AnyMatch(Equals('elephant'))
    matches_matches = [
        ['grass', 'cow', 'steak', 'milk', 'elephant'],
        (13, 'elephant'),
        ['elephant', 'elephant', 'elephant'],
        set(['hippo', 'rhino', 'elephant']),
        ]
    matches_mismatches = [
        [],
        ['grass', 'cow', 'steak', 'milk'],
        (13, 12, 10),
        ['element', 'hephalump', 'pachyderm'],
        set(['hippo', 'rhino', 'diplodocus']),
        ]

    str_examples = [
        ("AnyMatch(Equals('elephant'))", AnyMatch(Equals('elephant'))),
        ]

    describe_examples = [
        ('Differences: [\n'
         '7 != 11\n'
         '7 != 9\n'
         '7 != 10\n'
         ']',
         [11, 9, 10],
         AnyMatch(Equals(7))),
        ]


class TestAfterPreprocessing(TestCase, TestMatchersInterface):

    def parity(x):
        return x % 2

    matches_matcher = AfterPreprocessing(parity, Equals(1))
    matches_matches = [3, 5]
    matches_mismatches = [2]

    str_examples = [
        ("AfterPreprocessing(<function parity>, Equals(1))",
         AfterPreprocessing(parity, Equals(1))),
        ]

    describe_examples = [
        ("1 != 0: after <function parity> on 2", 2,
         AfterPreprocessing(parity, Equals(1))),
        ("1 != 0", 2,
         AfterPreprocessing(parity, Equals(1), annotate=False)),
        ]

class TestMatchersAnyInterface(TestCase, TestMatchersInterface):

    matches_matcher = MatchesAny(DocTestMatches("1"), DocTestMatches("2"))
    matches_matches = ["1", "2"]
    matches_mismatches = ["3"]

    str_examples = [(
        "MatchesAny(DocTestMatches('1\\n'), DocTestMatches('2\\n'))",
        MatchesAny(DocTestMatches("1"), DocTestMatches("2"))),
        ]

    describe_examples = [("""Differences: [
Expected:
    1
Got:
    3

Expected:
    2
Got:
    3

]""",
        "3", MatchesAny(DocTestMatches("1"), DocTestMatches("2")))]


class TestMatchesAllInterface(TestCase, TestMatchersInterface):

    matches_matcher = MatchesAll(NotEquals(1), NotEquals(2))
    matches_matches = [3, 4]
    matches_mismatches = [1, 2]

    str_examples = [
        ("MatchesAll(NotEquals(1), NotEquals(2))",
         MatchesAll(NotEquals(1), NotEquals(2)))]

    describe_examples = [
        ("""Differences: [
1 == 1
]""",
         1, MatchesAll(NotEquals(1), NotEquals(2))),
        ("1 == 1", 1,
         MatchesAll(NotEquals(2), NotEquals(1), Equals(3), first_only=True)),
        ]


class TestAnnotate(TestCase, TestMatchersInterface):

    matches_matcher = Annotate("foo", Equals(1))
    matches_matches = [1]
    matches_mismatches = [2]

    str_examples = [
        ("Annotate('foo', Equals(1))", Annotate("foo", Equals(1)))]

    describe_examples = [("1 != 2: foo", 2, Annotate('foo', Equals(1)))]

    def test_if_message_no_message(self):
        # Annotate.if_message returns the given matcher if there is no
        # message.
        matcher = Equals(1)
        not_annotated = Annotate.if_message('', matcher)
        self.assertIs(matcher, not_annotated)

    def test_if_message_given_message(self):
        # Annotate.if_message returns an annotated version of the matcher if a
        # message is provided.
        matcher = Equals(1)
        expected = Annotate('foo', matcher)
        annotated = Annotate.if_message('foo', matcher)
        self.assertThat(
            annotated,
            MatchesStructure.fromExample(expected, 'annotation', 'matcher'))


class TestAnnotatedMismatch(TestCase):

    run_tests_with = FullStackRunTest

    def test_forwards_details(self):
        x = Mismatch('description', {'foo': 'bar'})
        annotated = AnnotatedMismatch("annotation", x)
        self.assertEqual(x.get_details(), annotated.get_details())


class TestNotInterface(TestCase, TestMatchersInterface):

    matches_matcher = Not(Equals(1))
    matches_matches = [2]
    matches_mismatches = [1]

    str_examples = [
        ("Not(Equals(1))", Not(Equals(1))),
        ("Not(Equals('1'))", Not(Equals('1')))]

    describe_examples = [('1 matches Equals(1)', 1, Not(Equals(1)))]


def is_even(x):
    return x % 2 == 0


class TestMatchesPredicate(TestCase, TestMatchersInterface):

    matches_matcher = MatchesPredicate(is_even, "%s is not even")
    matches_matches = [2, 4, 6, 8]
    matches_mismatches = [3, 5, 7, 9]

    str_examples = [
        ("MatchesPredicate(%r, %r)" % (is_even, "%s is not even"),
         MatchesPredicate(is_even, "%s is not even")),
        ]

    describe_examples = [
        ('7 is not even', 7, MatchesPredicate(is_even, "%s is not even")),
        ]


def between(x, low, high):
    return low < x < high


class TestMatchesPredicateWithParams(TestCase, TestMatchersInterface):

    matches_matcher = MatchesPredicateWithParams(
        between, "{0} is not between {1} and {2}")(1, 9)
    matches_matches = [2, 4, 6, 8]
    matches_mismatches = [0, 1, 9, 10]

    str_examples = [
        ("MatchesPredicateWithParams(%r, %r)(%s)" % (
            between, "{0} is not between {1} and {2}", "1, 2"),
         MatchesPredicateWithParams(
            between, "{0} is not between {1} and {2}")(1, 2)),
        ("Between(1, 2)", MatchesPredicateWithParams(
            between, "{0} is not between {1} and {2}", "Between")(1, 2)),
        ]

    describe_examples = [
        ('1 is not between 2 and 3', 1, MatchesPredicateWithParams(
            between, "{0} is not between {1} and {2}")(2, 3)),
        ]


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
