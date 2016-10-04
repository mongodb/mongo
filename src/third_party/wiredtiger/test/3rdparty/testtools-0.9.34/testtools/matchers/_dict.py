# Copyright (c) 2009-2012 testtools developers. See LICENSE for details.

__all__ = [
    'KeysEqual',
    ]

from ..helpers import (
    dict_subtract,
    filter_values,
    map_values,
    )
from ._higherorder import (
    AnnotatedMismatch,
    PrefixedMismatch,
    MismatchesAll,
    )
from ._impl import Matcher, Mismatch


def LabelledMismatches(mismatches, details=None):
    """A collection of mismatches, each labelled."""
    return MismatchesAll(
        (PrefixedMismatch(k, v) for (k, v) in sorted(mismatches.items())),
        wrap=False)


class MatchesAllDict(Matcher):
    """Matches if all of the matchers it is created with match.

    A lot like ``MatchesAll``, but takes a dict of Matchers and labels any
    mismatches with the key of the dictionary.
    """

    def __init__(self, matchers):
        super(MatchesAllDict, self).__init__()
        self.matchers = matchers

    def __str__(self):
        return 'MatchesAllDict(%s)' % (_format_matcher_dict(self.matchers),)

    def match(self, observed):
        mismatches = {}
        for label in self.matchers:
            mismatches[label] = self.matchers[label].match(observed)
        return _dict_to_mismatch(
            mismatches, result_mismatch=LabelledMismatches)


class DictMismatches(Mismatch):
    """A mismatch with a dict of child mismatches."""

    def __init__(self, mismatches, details=None):
        super(DictMismatches, self).__init__(None, details=details)
        self.mismatches = mismatches

    def describe(self):
        lines = ['{']
        lines.extend(
            ['  %r: %s,' % (key, mismatch.describe())
             for (key, mismatch) in sorted(self.mismatches.items())])
        lines.append('}')
        return '\n'.join(lines)


def _dict_to_mismatch(data, to_mismatch=None,
                      result_mismatch=DictMismatches):
    if to_mismatch:
        data = map_values(to_mismatch, data)
    mismatches = filter_values(bool, data)
    if mismatches:
        return result_mismatch(mismatches)


class _MatchCommonKeys(Matcher):
    """Match on keys in a dictionary.

    Given a dictionary where the values are matchers, this will look for
    common keys in the matched dictionary and match if and only if all common
    keys match the given matchers.

    Thus::

      >>> structure = {'a': Equals('x'), 'b': Equals('y')}
      >>> _MatchCommonKeys(structure).match({'a': 'x', 'c': 'z'})
      None
    """

    def __init__(self, dict_of_matchers):
        super(_MatchCommonKeys, self).__init__()
        self._matchers = dict_of_matchers

    def _compare_dicts(self, expected, observed):
        common_keys = set(expected.keys()) & set(observed.keys())
        mismatches = {}
        for key in common_keys:
            mismatch = expected[key].match(observed[key])
            if mismatch:
                mismatches[key] = mismatch
        return mismatches

    def match(self, observed):
        mismatches = self._compare_dicts(self._matchers, observed)
        if mismatches:
            return DictMismatches(mismatches)


class _SubDictOf(Matcher):
    """Matches if the matched dict only has keys that are in given dict."""

    def __init__(self, super_dict, format_value=repr):
        super(_SubDictOf, self).__init__()
        self.super_dict = super_dict
        self.format_value = format_value

    def match(self, observed):
        excess = dict_subtract(observed, self.super_dict)
        return _dict_to_mismatch(
            excess, lambda v: Mismatch(self.format_value(v)))


class _SuperDictOf(Matcher):
    """Matches if all of the keys in the given dict are in the matched dict.
    """

    def __init__(self, sub_dict, format_value=repr):
        super(_SuperDictOf, self).__init__()
        self.sub_dict = sub_dict
        self.format_value = format_value

    def match(self, super_dict):
        return _SubDictOf(super_dict, self.format_value).match(self.sub_dict)


def _format_matcher_dict(matchers):
    return '{%s}' % (
        ', '.join(sorted('%r: %s' % (k, v) for k, v in matchers.items())))


class _CombinedMatcher(Matcher):
    """Many matchers labelled and combined into one uber-matcher.

    Subclass this and then specify a dict of matcher factories that take a
    single 'expected' value and return a matcher.  The subclass will match
    only if all of the matchers made from factories match.

    Not **entirely** dissimilar from ``MatchesAll``.
    """

    matcher_factories = {}

    def __init__(self, expected):
        super(_CombinedMatcher, self).__init__()
        self._expected = expected

    def format_expected(self, expected):
        return repr(expected)

    def __str__(self):
        return '%s(%s)' % (
            self.__class__.__name__, self.format_expected(self._expected))

    def match(self, observed):
        matchers = dict(
            (k, v(self._expected)) for k, v in self.matcher_factories.items())
        return MatchesAllDict(matchers).match(observed)


class MatchesDict(_CombinedMatcher):
    """Match a dictionary exactly, by its keys.

    Specify a dictionary mapping keys (often strings) to matchers.  This is
    the 'expected' dict.  Any dictionary that matches this must have exactly
    the same keys, and the values must match the corresponding matchers in the
    expected dict.
    """

    matcher_factories = {
        'Extra': _SubDictOf,
        'Missing': lambda m: _SuperDictOf(m, format_value=str),
        'Differences': _MatchCommonKeys,
        }

    format_expected = lambda self, expected: _format_matcher_dict(expected)


class ContainsDict(_CombinedMatcher):
    """Match a dictionary for that contains a specified sub-dictionary.

    Specify a dictionary mapping keys (often strings) to matchers.  This is
    the 'expected' dict.  Any dictionary that matches this must have **at
    least** these keys, and the values must match the corresponding matchers
    in the expected dict.  Dictionaries that have more keys will also match.

    In other words, any matching dictionary must contain the dictionary given
    to the constructor.

    Does not check for strict sub-dictionary.  That is, equal dictionaries
    match.
    """

    matcher_factories = {
        'Missing': lambda m: _SuperDictOf(m, format_value=str),
        'Differences': _MatchCommonKeys,
        }

    format_expected = lambda self, expected: _format_matcher_dict(expected)


class ContainedByDict(_CombinedMatcher):
    """Match a dictionary for which this is a super-dictionary.

    Specify a dictionary mapping keys (often strings) to matchers.  This is
    the 'expected' dict.  Any dictionary that matches this must have **only**
    these keys, and the values must match the corresponding matchers in the
    expected dict.  Dictionaries that have fewer keys can also match.

    In other words, any matching dictionary must be contained by the
    dictionary given to the constructor.

    Does not check for strict super-dictionary.  That is, equal dictionaries
    match.
    """

    matcher_factories = {
        'Extra': _SubDictOf,
        'Differences': _MatchCommonKeys,
        }

    format_expected = lambda self, expected: _format_matcher_dict(expected)


class KeysEqual(Matcher):
    """Checks whether a dict has particular keys."""

    def __init__(self, *expected):
        """Create a `KeysEqual` Matcher.

        :param expected: The keys the dict is expected to have.  If a dict,
            then we use the keys of that dict, if a collection, we assume it
            is a collection of expected keys.
        """
        super(KeysEqual, self).__init__()
        try:
            self.expected = expected[0].keys()
        except AttributeError:
            self.expected = list(expected)

    def __str__(self):
        return "KeysEqual(%s)" % ', '.join(map(repr, self.expected))

    def match(self, matchee):
        from ._basic import _BinaryMismatch, Equals
        expected = sorted(self.expected)
        matched = Equals(expected).match(sorted(matchee.keys()))
        if matched:
            return AnnotatedMismatch(
                'Keys not equal',
                _BinaryMismatch(expected, 'does not match', matchee))
        return None
