# Copyright (c) 2009-2012 testtools developers. See LICENSE for details.

__all__ = [
    'Contains',
    'EndsWith',
    'Equals',
    'GreaterThan',
    'HasLength',
    'Is',
    'IsInstance',
    'LessThan',
    'MatchesRegex',
    'NotEquals',
    'SameMembers',
    'StartsWith',
    ]

import operator
from pprint import pformat
import re
import warnings

from ..compat import (
    text_repr,
    )
from ..helpers import list_subtract
from ._higherorder import (
    MatchesPredicateWithParams,
    PostfixedMismatch,
    )
from ._impl import (
    Matcher,
    Mismatch,
    )


def _format(thing):
    """
    Blocks of text with newlines are formatted as triple-quote
    strings. Everything else is pretty-printed.
    """
    if isinstance(thing, (str, bytes)):
        return text_repr(thing)
    return pformat(thing)


class _BinaryComparison:
    """Matcher that compares an object to another object."""

    def __init__(self, expected):
        self.expected = expected

    def __str__(self):
        return f"{self.__class__.__name__}({self.expected!r})"

    def match(self, other):
        if self.comparator(other, self.expected):
            return None
        return _BinaryMismatch(other, self.mismatch_string, self.expected)

    def comparator(self, expected, other):
        raise NotImplementedError(self.comparator)


class _BinaryMismatch(Mismatch):
    """Two things did not match."""

    def __init__(self, actual, mismatch_string, reference,
                 reference_on_right=True):
        self._actual = actual
        self._mismatch_string = mismatch_string
        self._reference = reference
        self._reference_on_right = reference_on_right

    @property
    def expected(self):
        warnings.warn(
            f'{self.__class__.__name__}.expected deprecated after 1.8.1',
            DeprecationWarning,
            stacklevel=2,
        )
        return self._reference

    @property
    def other(self):
        warnings.warn(
            f'{self.__class__.__name__}.other deprecated after 1.8.1',
            DeprecationWarning,
            stacklevel=2,
        )
        return self._actual

    def describe(self):
        actual = repr(self._actual)
        reference = repr(self._reference)
        if len(actual) + len(reference) > 70:
            return "{}:\nreference = {}\nactual    = {}\n".format(
                self._mismatch_string, _format(self._reference),
                _format(self._actual))
        else:
            if self._reference_on_right:
                left, right = actual, reference
            else:
                left, right = reference, actual
            return f"{left} {self._mismatch_string} {right}"


class Equals(_BinaryComparison):
    """Matches if the items are equal."""

    comparator = operator.eq
    mismatch_string = '!='


class _FlippedEquals:
    """Matches if the items are equal.

    Exactly like ``Equals`` except that the short mismatch message is "
    $reference != $actual" rather than "$actual != $reference". This allows
    for ``TestCase.assertEqual`` to use a matcher but still have the order of
    items in the error message align with the order of items in the call to
    the assertion.
    """

    def __init__(self, expected):
        self._expected = expected

    def match(self, other):
        mismatch = Equals(self._expected).match(other)
        if not mismatch:
            return None
        return _BinaryMismatch(other, '!=', self._expected, False)


class NotEquals(_BinaryComparison):
    """Matches if the items are not equal.

    In most cases, this is equivalent to ``Not(Equals(foo))``. The difference
    only matters when testing ``__ne__`` implementations.
    """

    comparator = operator.ne
    mismatch_string = '=='


class Is(_BinaryComparison):
    """Matches if the items are identical."""

    comparator = operator.is_
    mismatch_string = 'is not'


class LessThan(_BinaryComparison):
    """Matches if the item is less than the matchers reference object."""

    comparator = operator.__lt__
    mismatch_string = '>='


class GreaterThan(_BinaryComparison):
    """Matches if the item is greater than the matchers reference object."""

    comparator = operator.__gt__
    mismatch_string = '<='


class SameMembers(Matcher):
    """Matches if two iterators have the same members.

    This is not the same as set equivalence.  The two iterators must be of the
    same length and have the same repetitions.
    """

    def __init__(self, expected):
        super().__init__()
        self.expected = expected

    def __str__(self):
        return f'{self.__class__.__name__}({self.expected!r})'

    def match(self, observed):
        expected_only = list_subtract(self.expected, observed)
        observed_only = list_subtract(observed, self.expected)
        if expected_only == observed_only == []:
            return
        return PostfixedMismatch(
            "\nmissing:    {}\nextra:      {}".format(
                _format(expected_only), _format(observed_only)),
            _BinaryMismatch(observed, 'elements differ', self.expected))


class DoesNotStartWith(Mismatch):

    def __init__(self, matchee, expected):
        """Create a DoesNotStartWith Mismatch.

        :param matchee: the string that did not match.
        :param expected: the string that 'matchee' was expected to start with.
        """
        self.matchee = matchee
        self.expected = expected

    def describe(self):
        return "{} does not start with {}.".format(
            text_repr(self.matchee), text_repr(self.expected))


class StartsWith(Matcher):
    """Checks whether one string starts with another."""

    def __init__(self, expected):
        """Create a StartsWith Matcher.

        :param expected: the string that matchees should start with.
        """
        self.expected = expected

    def __str__(self):
        return f"StartsWith({self.expected!r})"

    def match(self, matchee):
        if not matchee.startswith(self.expected):
            return DoesNotStartWith(matchee, self.expected)
        return None


class DoesNotEndWith(Mismatch):

    def __init__(self, matchee, expected):
        """Create a DoesNotEndWith Mismatch.

        :param matchee: the string that did not match.
        :param expected: the string that 'matchee' was expected to end with.
        """
        self.matchee = matchee
        self.expected = expected

    def describe(self):
        return "{} does not end with {}.".format(
            text_repr(self.matchee), text_repr(self.expected))


class EndsWith(Matcher):
    """Checks whether one string ends with another."""

    def __init__(self, expected):
        """Create a EndsWith Matcher.

        :param expected: the string that matchees should end with.
        """
        self.expected = expected

    def __str__(self):
        return f"EndsWith({self.expected!r})"

    def match(self, matchee):
        if not matchee.endswith(self.expected):
            return DoesNotEndWith(matchee, self.expected)
        return None


class IsInstance:
    """Matcher that wraps isinstance."""

    def __init__(self, *types):
        self.types = tuple(types)

    def __str__(self):
        return "{}({})".format(self.__class__.__name__,
                ', '.join(type.__name__ for type in self.types))

    def match(self, other):
        if isinstance(other, self.types):
            return None
        return NotAnInstance(other, self.types)


class NotAnInstance(Mismatch):

    def __init__(self, matchee, types):
        """Create a NotAnInstance Mismatch.

        :param matchee: the thing which is not an instance of any of types.
        :param types: A tuple of the types which were expected.
        """
        self.matchee = matchee
        self.types = types

    def describe(self):
        if len(self.types) == 1:
            typestr = self.types[0].__name__
        else:
            typestr = 'any of (%s)' % ', '.join(type.__name__ for type in
                    self.types)
        return f"'{self.matchee}' is not an instance of {typestr}"


class DoesNotContain(Mismatch):

    def __init__(self, matchee, needle):
        """Create a DoesNotContain Mismatch.

        :param matchee: the object that did not contain needle.
        :param needle: the needle that 'matchee' was expected to contain.
        """
        self.matchee = matchee
        self.needle = needle

    def describe(self):
        return f"{self.needle!r} not in {self.matchee!r}"


class Contains(Matcher):
    """Checks whether something is contained in another thing."""

    def __init__(self, needle):
        """Create a Contains Matcher.

        :param needle: the thing that needs to be contained by matchees.
        """
        self.needle = needle

    def __str__(self):
        return f"Contains({self.needle!r})"

    def match(self, matchee):
        try:
            if self.needle not in matchee:
                return DoesNotContain(matchee, self.needle)
        except TypeError:
            # e.g. 1 in 2 will raise TypeError
            return DoesNotContain(matchee, self.needle)
        return None


class MatchesRegex:
    """Matches if the matchee is matched by a regular expression."""

    def __init__(self, pattern, flags=0):
        self.pattern = pattern
        self.flags = flags

    def __str__(self):
        args = ['%r' % self.pattern]
        flag_arg = []
        # dir() sorts the attributes for us, so we don't need to do it again.
        for flag in dir(re):
            if len(flag) == 1:
                if self.flags & getattr(re, flag):
                    flag_arg.append('re.%s' % flag)
        if flag_arg:
            args.append('|'.join(flag_arg))
        return '{}({})'.format(self.__class__.__name__, ', '.join(args))

    def match(self, value):
        if not re.match(self.pattern, value, self.flags):
            pattern = self.pattern
            if not isinstance(pattern, str):
                pattern = pattern.decode("latin1")
            pattern = pattern.encode("unicode_escape").decode("ascii")
            return Mismatch("{!r} does not match /{}/".format(
                    value, pattern.replace("\\\\", "\\")))


def has_len(x, y):
    return len(x) == y


HasLength = MatchesPredicateWithParams(has_len, "len({0}) != {1}", "HasLength")
