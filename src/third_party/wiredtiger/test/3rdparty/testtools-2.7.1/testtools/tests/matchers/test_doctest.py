# Copyright (c) 2008-2012 testtools developers. See LICENSE for details.

import doctest

from testtools import TestCase
from testtools.compat import (
    _b,
    )
from testtools.matchers._doctest import DocTestMatches
from testtools.tests.helpers import FullStackRunTest
from testtools.tests.matchers.helpers import TestMatchersInterface



class TestDocTestMatchesInterface(TestCase, TestMatchersInterface):

    matches_matcher = DocTestMatches("Ran 1 test in ...s", doctest.ELLIPSIS)
    matches_matches = ["Ran 1 test in 0.000s", "Ran 1 test in 1.234s"]
    matches_mismatches = ["Ran 1 tests in 0.000s", "Ran 2 test in 0.000s"]

    str_examples = [("DocTestMatches('Ran 1 test in ...s\\n')",
        DocTestMatches("Ran 1 test in ...s")),
        ("DocTestMatches('foo\\n', flags=8)", DocTestMatches("foo", flags=8)),
        ]

    describe_examples = [('Expected:\n    Ran 1 tests in ...s\nGot:\n'
        '    Ran 1 test in 0.123s\n', "Ran 1 test in 0.123s",
        DocTestMatches("Ran 1 tests in ...s", doctest.ELLIPSIS))]


class TestDocTestMatchesInterfaceUnicode(TestCase, TestMatchersInterface):

    matches_matcher = DocTestMatches("\xa7...", doctest.ELLIPSIS)
    matches_matches = ["\xa7", "\xa7 more\n"]
    matches_mismatches = ["\\xa7", "more \xa7", "\n\xa7"]

    str_examples = [("DocTestMatches({!r})".format("\xa7\n"),
        DocTestMatches("\xa7")),
        ]

    describe_examples = [(
        "Expected:\n    \xa7\nGot:\n    a\n",
        "a",
        DocTestMatches("\xa7", doctest.ELLIPSIS))]


class TestDocTestMatchesSpecific(TestCase):

    run_tests_with = FullStackRunTest

    def test___init__simple(self):
        matcher = DocTestMatches("foo")
        self.assertEqual("foo\n", matcher.want)

    def test___init__flags(self):
        matcher = DocTestMatches("bar\n", doctest.ELLIPSIS)
        self.assertEqual("bar\n", matcher.want)
        self.assertEqual(doctest.ELLIPSIS, matcher.flags)

    def test_describe_non_ascii_bytes(self):
        """Even with bytestrings, the mismatch should be coercible to unicode

        DocTestMatches is intended for text, but the Python 2 str type also
        permits arbitrary binary inputs. This is a slightly bogus thing to do,
        and under Python 3 using bytes objects will reasonably raise an error.
        """
        header = _b("\x89PNG\r\n\x1a\n...")
        self.assertRaises(TypeError,
            DocTestMatches, header, doctest.ELLIPSIS)


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
