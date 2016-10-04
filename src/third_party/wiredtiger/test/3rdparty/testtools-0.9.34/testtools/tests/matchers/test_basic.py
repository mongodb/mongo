# Copyright (c) 2008-2012 testtools developers. See LICENSE for details.

import re

from testtools import TestCase
from testtools.compat import (
    text_repr,
    _b,
    _u,
    )
from testtools.matchers._basic import (
    _BinaryMismatch,
    Contains,
    DoesNotEndWith,
    DoesNotStartWith,
    EndsWith,
    Equals,
    Is,
    IsInstance,
    LessThan,
    GreaterThan,
    HasLength,
    MatchesRegex,
    NotEquals,
    SameMembers,
    StartsWith,
    )
from testtools.tests.helpers import FullStackRunTest
from testtools.tests.matchers.helpers import TestMatchersInterface


class Test_BinaryMismatch(TestCase):
    """Mismatches from binary comparisons need useful describe output"""

    _long_string = "This is a longish multiline non-ascii string\n\xa7"
    _long_b = _b(_long_string)
    _long_u = _u(_long_string)

    class CustomRepr(object):
        def __init__(self, repr_string):
            self._repr_string = repr_string
        def __repr__(self):
            return _u('<object ') + _u(self._repr_string) + _u('>')

    def test_short_objects(self):
        o1, o2 = self.CustomRepr('a'), self.CustomRepr('b')
        mismatch = _BinaryMismatch(o1, "!~", o2)
        self.assertEqual(mismatch.describe(), "%r !~ %r" % (o1, o2))

    def test_short_mixed_strings(self):
        b, u = _b("\xa7"), _u("\xa7")
        mismatch = _BinaryMismatch(b, "!~", u)
        self.assertEqual(mismatch.describe(), "%r !~ %r" % (b, u))

    def test_long_bytes(self):
        one_line_b = self._long_b.replace(_b("\n"), _b(" "))
        mismatch = _BinaryMismatch(one_line_b, "!~", self._long_b)
        self.assertEqual(mismatch.describe(),
            "%s:\nreference = %s\nactual    = %s\n" % ("!~",
                text_repr(one_line_b),
                text_repr(self._long_b, multiline=True)))

    def test_long_unicode(self):
        one_line_u = self._long_u.replace("\n", " ")
        mismatch = _BinaryMismatch(one_line_u, "!~", self._long_u)
        self.assertEqual(mismatch.describe(),
            "%s:\nreference = %s\nactual    = %s\n" % ("!~",
                text_repr(one_line_u),
                text_repr(self._long_u, multiline=True)))

    def test_long_mixed_strings(self):
        mismatch = _BinaryMismatch(self._long_b, "!~", self._long_u)
        self.assertEqual(mismatch.describe(),
            "%s:\nreference = %s\nactual    = %s\n" % ("!~",
                text_repr(self._long_b, multiline=True),
                text_repr(self._long_u, multiline=True)))

    def test_long_bytes_and_object(self):
        obj = object()
        mismatch = _BinaryMismatch(self._long_b, "!~", obj)
        self.assertEqual(mismatch.describe(),
            "%s:\nreference = %s\nactual    = %s\n" % ("!~",
                text_repr(self._long_b, multiline=True),
                repr(obj)))

    def test_long_unicode_and_object(self):
        obj = object()
        mismatch = _BinaryMismatch(self._long_u, "!~", obj)
        self.assertEqual(mismatch.describe(),
            "%s:\nreference = %s\nactual    = %s\n" % ("!~",
                text_repr(self._long_u, multiline=True),
                repr(obj)))


class TestEqualsInterface(TestCase, TestMatchersInterface):

    matches_matcher = Equals(1)
    matches_matches = [1]
    matches_mismatches = [2]

    str_examples = [("Equals(1)", Equals(1)), ("Equals('1')", Equals('1'))]

    describe_examples = [("1 != 2", 2, Equals(1))]


class TestNotEqualsInterface(TestCase, TestMatchersInterface):

    matches_matcher = NotEquals(1)
    matches_matches = [2]
    matches_mismatches = [1]

    str_examples = [
        ("NotEquals(1)", NotEquals(1)), ("NotEquals('1')", NotEquals('1'))]

    describe_examples = [("1 == 1", 1, NotEquals(1))]


class TestIsInterface(TestCase, TestMatchersInterface):

    foo = object()
    bar = object()

    matches_matcher = Is(foo)
    matches_matches = [foo]
    matches_mismatches = [bar, 1]

    str_examples = [("Is(2)", Is(2))]

    describe_examples = [("1 is not 2", 2, Is(1))]


class TestIsInstanceInterface(TestCase, TestMatchersInterface):

    class Foo:pass

    matches_matcher = IsInstance(Foo)
    matches_matches = [Foo()]
    matches_mismatches = [object(), 1, Foo]

    str_examples = [
            ("IsInstance(str)", IsInstance(str)),
            ("IsInstance(str, int)", IsInstance(str, int)),
            ]

    describe_examples = [
            ("'foo' is not an instance of int", 'foo', IsInstance(int)),
            ("'foo' is not an instance of any of (int, type)", 'foo',
             IsInstance(int, type)),
            ]


class TestLessThanInterface(TestCase, TestMatchersInterface):

    matches_matcher = LessThan(4)
    matches_matches = [-5, 3]
    matches_mismatches = [4, 5, 5000]

    str_examples = [
        ("LessThan(12)", LessThan(12)),
        ]

    describe_examples = [
        ('4 is not > 5', 5, LessThan(4)),
        ('4 is not > 4', 4, LessThan(4)),
        ]


class TestGreaterThanInterface(TestCase, TestMatchersInterface):

    matches_matcher = GreaterThan(4)
    matches_matches = [5, 8]
    matches_mismatches = [-2, 0, 4]

    str_examples = [
        ("GreaterThan(12)", GreaterThan(12)),
        ]

    describe_examples = [
        ('5 is not < 4', 4, GreaterThan(5)),
        ('4 is not < 4', 4, GreaterThan(4)),
        ]


class TestContainsInterface(TestCase, TestMatchersInterface):

    matches_matcher = Contains('foo')
    matches_matches = ['foo', 'afoo', 'fooa']
    matches_mismatches = ['f', 'fo', 'oo', 'faoo', 'foao']

    str_examples = [
        ("Contains(1)", Contains(1)),
        ("Contains('foo')", Contains('foo')),
        ]

    describe_examples = [("1 not in 2", 2, Contains(1))]


class DoesNotStartWithTests(TestCase):

    run_tests_with = FullStackRunTest

    def test_describe(self):
        mismatch = DoesNotStartWith("fo", "bo")
        self.assertEqual("'fo' does not start with 'bo'.", mismatch.describe())

    def test_describe_non_ascii_unicode(self):
        string = _u("A\xA7")
        suffix = _u("B\xA7")
        mismatch = DoesNotStartWith(string, suffix)
        self.assertEqual("%s does not start with %s." % (
            text_repr(string), text_repr(suffix)),
            mismatch.describe())

    def test_describe_non_ascii_bytes(self):
        string = _b("A\xA7")
        suffix = _b("B\xA7")
        mismatch = DoesNotStartWith(string, suffix)
        self.assertEqual("%r does not start with %r." % (string, suffix),
            mismatch.describe())


class StartsWithTests(TestCase):

    run_tests_with = FullStackRunTest

    def test_str(self):
        matcher = StartsWith("bar")
        self.assertEqual("StartsWith('bar')", str(matcher))

    def test_str_with_bytes(self):
        b = _b("\xA7")
        matcher = StartsWith(b)
        self.assertEqual("StartsWith(%r)" % (b,), str(matcher))

    def test_str_with_unicode(self):
        u = _u("\xA7")
        matcher = StartsWith(u)
        self.assertEqual("StartsWith(%r)" % (u,), str(matcher))

    def test_match(self):
        matcher = StartsWith("bar")
        self.assertIs(None, matcher.match("barf"))

    def test_mismatch_returns_does_not_start_with(self):
        matcher = StartsWith("bar")
        self.assertIsInstance(matcher.match("foo"), DoesNotStartWith)

    def test_mismatch_sets_matchee(self):
        matcher = StartsWith("bar")
        mismatch = matcher.match("foo")
        self.assertEqual("foo", mismatch.matchee)

    def test_mismatch_sets_expected(self):
        matcher = StartsWith("bar")
        mismatch = matcher.match("foo")
        self.assertEqual("bar", mismatch.expected)


class DoesNotEndWithTests(TestCase):

    run_tests_with = FullStackRunTest

    def test_describe(self):
        mismatch = DoesNotEndWith("fo", "bo")
        self.assertEqual("'fo' does not end with 'bo'.", mismatch.describe())

    def test_describe_non_ascii_unicode(self):
        string = _u("A\xA7")
        suffix = _u("B\xA7")
        mismatch = DoesNotEndWith(string, suffix)
        self.assertEqual("%s does not end with %s." % (
            text_repr(string), text_repr(suffix)),
            mismatch.describe())

    def test_describe_non_ascii_bytes(self):
        string = _b("A\xA7")
        suffix = _b("B\xA7")
        mismatch = DoesNotEndWith(string, suffix)
        self.assertEqual("%r does not end with %r." % (string, suffix),
            mismatch.describe())


class EndsWithTests(TestCase):

    run_tests_with = FullStackRunTest

    def test_str(self):
        matcher = EndsWith("bar")
        self.assertEqual("EndsWith('bar')", str(matcher))

    def test_str_with_bytes(self):
        b = _b("\xA7")
        matcher = EndsWith(b)
        self.assertEqual("EndsWith(%r)" % (b,), str(matcher))

    def test_str_with_unicode(self):
        u = _u("\xA7")
        matcher = EndsWith(u)
        self.assertEqual("EndsWith(%r)" % (u,), str(matcher))

    def test_match(self):
        matcher = EndsWith("arf")
        self.assertIs(None, matcher.match("barf"))

    def test_mismatch_returns_does_not_end_with(self):
        matcher = EndsWith("bar")
        self.assertIsInstance(matcher.match("foo"), DoesNotEndWith)

    def test_mismatch_sets_matchee(self):
        matcher = EndsWith("bar")
        mismatch = matcher.match("foo")
        self.assertEqual("foo", mismatch.matchee)

    def test_mismatch_sets_expected(self):
        matcher = EndsWith("bar")
        mismatch = matcher.match("foo")
        self.assertEqual("bar", mismatch.expected)


class TestSameMembers(TestCase, TestMatchersInterface):

    matches_matcher = SameMembers([1, 1, 2, 3, {'foo': 'bar'}])
    matches_matches = [
        [1, 1, 2, 3, {'foo': 'bar'}],
        [3, {'foo': 'bar'}, 1, 2, 1],
        [3, 2, 1, {'foo': 'bar'}, 1],
        (2, {'foo': 'bar'}, 3, 1, 1),
        ]
    matches_mismatches = [
        set([1, 2, 3]),
        [1, 1, 2, 3, 5],
        [1, 2, 3, {'foo': 'bar'}],
        'foo',
        ]

    describe_examples = [
        (("elements differ:\n"
          "reference = ['apple', 'orange', 'canteloupe', 'watermelon', 'lemon', 'banana']\n"
          "actual    = ['orange', 'apple', 'banana', 'sparrow', 'lemon', 'canteloupe']\n"
          ": \n"
          "missing:    ['watermelon']\n"
          "extra:      ['sparrow']"
          ),
         ['orange', 'apple', 'banana', 'sparrow', 'lemon', 'canteloupe',],
         SameMembers(
             ['apple', 'orange', 'canteloupe', 'watermelon',
              'lemon', 'banana',])),
        ]

    str_examples = [
        ('SameMembers([1, 2, 3])', SameMembers([1, 2, 3])),
        ]


class TestMatchesRegex(TestCase, TestMatchersInterface):

    matches_matcher = MatchesRegex('a|b')
    matches_matches = ['a', 'b']
    matches_mismatches = ['c']

    str_examples = [
        ("MatchesRegex('a|b')", MatchesRegex('a|b')),
        ("MatchesRegex('a|b', re.M)", MatchesRegex('a|b', re.M)),
        ("MatchesRegex('a|b', re.I|re.M)", MatchesRegex('a|b', re.I|re.M)),
        ("MatchesRegex(%r)" % (_b("\xA7"),), MatchesRegex(_b("\xA7"))),
        ("MatchesRegex(%r)" % (_u("\xA7"),), MatchesRegex(_u("\xA7"))),
        ]

    describe_examples = [
        ("'c' does not match /a|b/", 'c', MatchesRegex('a|b')),
        ("'c' does not match /a\d/", 'c', MatchesRegex(r'a\d')),
        ("%r does not match /\\s+\\xa7/" % (_b('c'),),
            _b('c'), MatchesRegex(_b("\\s+\xA7"))),
        ("%r does not match /\\s+\\xa7/" % (_u('c'),),
            _u('c'), MatchesRegex(_u("\\s+\xA7"))),
        ]


class TestHasLength(TestCase, TestMatchersInterface):

    matches_matcher = HasLength(2)
    matches_matches = [[1, 2]]
    matches_mismatches = [[], [1], [3, 2, 1]]

    str_examples = [
        ("HasLength(2)", HasLength(2)),
        ]

    describe_examples = [
        ("len([]) != 1", [], HasLength(1)),
        ]


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
