from testtools import TestCase
from testtools.matchers import (
    Equals,
    NotEquals,
    Not,
    )
from testtools.matchers._dict import (
    ContainedByDict,
    ContainsDict,
    KeysEqual,
    MatchesAllDict,
    MatchesDict,
    _SubDictOf,
    )
from testtools.tests.matchers.helpers import TestMatchersInterface


class TestMatchesAllDictInterface(TestCase, TestMatchersInterface):

    matches_matcher = MatchesAllDict({'a': NotEquals(1), 'b': NotEquals(2)})
    matches_matches = [3, 4]
    matches_mismatches = [1, 2]

    str_examples = [
        ("MatchesAllDict({'a': NotEquals(1), 'b': NotEquals(2)})",
         matches_matcher)]

    describe_examples = [
        ("""a: 1 == 1""", 1, matches_matcher),
        ]


class TestKeysEqualWithList(TestCase, TestMatchersInterface):

    matches_matcher = KeysEqual('foo', 'bar')
    matches_matches = [
        {'foo': 0, 'bar': 1},
        ]
    matches_mismatches = [
        {},
        {'foo': 0},
        {'bar': 1},
        {'foo': 0, 'bar': 1, 'baz': 2},
        {'a': None, 'b': None, 'c': None},
        ]

    str_examples = [
        ("KeysEqual('foo', 'bar')", KeysEqual('foo', 'bar')),
        ]

    describe_examples = []

    def test_description(self):
        matchee = {'foo': 0, 'bar': 1, 'baz': 2}
        mismatch = KeysEqual('foo', 'bar').match(matchee)
        description = mismatch.describe()
        self.assertThat(
            description, Equals(
                "['bar', 'foo'] does not match %r: Keys not equal"
                % (matchee,)))


class TestKeysEqualWithDict(TestKeysEqualWithList):

    matches_matcher = KeysEqual({'foo': 3, 'bar': 4})


class TestSubDictOf(TestCase, TestMatchersInterface):

    matches_matcher = _SubDictOf({'foo': 'bar', 'baz': 'qux'})

    matches_matches = [
        {'foo': 'bar', 'baz': 'qux'},
        {'foo': 'bar'},
        ]

    matches_mismatches = [
        {'foo': 'bar', 'baz': 'qux', 'cat': 'dog'},
        {'foo': 'bar', 'cat': 'dog'},
        ]

    str_examples = []
    describe_examples = []


class TestMatchesDict(TestCase, TestMatchersInterface):

    matches_matcher = MatchesDict(
        {'foo': Equals('bar'), 'baz': Not(Equals('qux'))})

    matches_matches = [
        {'foo': 'bar', 'baz': None},
        {'foo': 'bar', 'baz': 'quux'},
        ]
    matches_mismatches = [
        {},
        {'foo': 'bar', 'baz': 'qux'},
        {'foo': 'bop', 'baz': 'qux'},
        {'foo': 'bar', 'baz': 'quux', 'cat': 'dog'},
        {'foo': 'bar', 'cat': 'dog'},
        ]

    str_examples = [
        ("MatchesDict({'baz': %s, 'foo': %s})" % (
                Not(Equals('qux')), Equals('bar')),
         matches_matcher),
        ]

    describe_examples = [
        ("Missing: {\n"
         "  'baz': Not(Equals('qux')),\n"
         "  'foo': Equals('bar'),\n"
         "}",
         {}, matches_matcher),
        ("Differences: {\n"
         "  'baz': 'qux' matches Equals('qux'),\n"
         "}",
         {'foo': 'bar', 'baz': 'qux'}, matches_matcher),
        ("Differences: {\n"
         "  'baz': 'qux' matches Equals('qux'),\n"
         "  'foo': 'bar' != 'bop',\n"
         "}",
         {'foo': 'bop', 'baz': 'qux'}, matches_matcher),
        ("Extra: {\n"
         "  'cat': 'dog',\n"
         "}",
         {'foo': 'bar', 'baz': 'quux', 'cat': 'dog'}, matches_matcher),
        ("Extra: {\n"
         "  'cat': 'dog',\n"
         "}\n"
         "Missing: {\n"
         "  'baz': Not(Equals('qux')),\n"
         "}",
         {'foo': 'bar', 'cat': 'dog'}, matches_matcher),
        ]


class TestContainsDict(TestCase, TestMatchersInterface):

    matches_matcher = ContainsDict(
        {'foo': Equals('bar'), 'baz': Not(Equals('qux'))})

    matches_matches = [
        {'foo': 'bar', 'baz': None},
        {'foo': 'bar', 'baz': 'quux'},
        {'foo': 'bar', 'baz': 'quux', 'cat': 'dog'},
        ]
    matches_mismatches = [
        {},
        {'foo': 'bar', 'baz': 'qux'},
        {'foo': 'bop', 'baz': 'qux'},
        {'foo': 'bar', 'cat': 'dog'},
        {'foo': 'bar'},
        ]

    str_examples = [
        ("ContainsDict({'baz': %s, 'foo': %s})" % (
                Not(Equals('qux')), Equals('bar')),
         matches_matcher),
        ]

    describe_examples = [
        ("Missing: {\n"
         "  'baz': Not(Equals('qux')),\n"
         "  'foo': Equals('bar'),\n"
         "}",
         {}, matches_matcher),
        ("Differences: {\n"
         "  'baz': 'qux' matches Equals('qux'),\n"
         "}",
         {'foo': 'bar', 'baz': 'qux'}, matches_matcher),
        ("Differences: {\n"
         "  'baz': 'qux' matches Equals('qux'),\n"
         "  'foo': 'bar' != 'bop',\n"
         "}",
         {'foo': 'bop', 'baz': 'qux'}, matches_matcher),
        ("Missing: {\n"
         "  'baz': Not(Equals('qux')),\n"
         "}",
         {'foo': 'bar', 'cat': 'dog'}, matches_matcher),
        ]


class TestContainedByDict(TestCase, TestMatchersInterface):

    matches_matcher = ContainedByDict(
        {'foo': Equals('bar'), 'baz': Not(Equals('qux'))})

    matches_matches = [
        {},
        {'foo': 'bar'},
        {'foo': 'bar', 'baz': 'quux'},
        {'baz': 'quux'},
        ]
    matches_mismatches = [
        {'foo': 'bar', 'baz': 'quux', 'cat': 'dog'},
        {'foo': 'bar', 'baz': 'qux'},
        {'foo': 'bop', 'baz': 'qux'},
        {'foo': 'bar', 'cat': 'dog'},
        ]

    str_examples = [
        ("ContainedByDict({'baz': %s, 'foo': %s})" % (
                Not(Equals('qux')), Equals('bar')),
         matches_matcher),
        ]

    describe_examples = [
        ("Differences: {\n"
         "  'baz': 'qux' matches Equals('qux'),\n"
         "}",
         {'foo': 'bar', 'baz': 'qux'}, matches_matcher),
        ("Differences: {\n"
         "  'baz': 'qux' matches Equals('qux'),\n"
         "  'foo': 'bar' != 'bop',\n"
         "}",
         {'foo': 'bop', 'baz': 'qux'}, matches_matcher),
        ("Extra: {\n"
         "  'cat': 'dog',\n"
         "}",
         {'foo': 'bar', 'cat': 'dog'}, matches_matcher),
        ]


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
