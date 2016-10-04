# Copyright (c) 2009-2012 testtools developers. See LICENSE for details.

__all__ = [
    'ContainsAll',
    'MatchesListwise',
    'MatchesSetwise',
    'MatchesStructure',
    ]

"""Matchers that operate with knowledge of Python data structures."""

from ..helpers import map_values
from ._higherorder import (
    Annotate,
    MatchesAll,
    MismatchesAll,
    )
from ._impl import Mismatch


def ContainsAll(items):
    """Make a matcher that checks whether a list of things is contained
    in another thing.

    The matcher effectively checks that the provided sequence is a subset of
    the matchee.
    """
    from ._basic import Contains
    return MatchesAll(*map(Contains, items), first_only=False)


class MatchesListwise(object):
    """Matches if each matcher matches the corresponding value.

    More easily explained by example than in words:

    >>> from ._basic import Equals
    >>> MatchesListwise([Equals(1)]).match([1])
    >>> MatchesListwise([Equals(1), Equals(2)]).match([1, 2])
    >>> print (MatchesListwise([Equals(1), Equals(2)]).match([2, 1]).describe())
    Differences: [
    1 != 2
    2 != 1
    ]
    >>> matcher = MatchesListwise([Equals(1), Equals(2)], first_only=True)
    >>> print (matcher.match([3, 4]).describe())
    1 != 3
    """

    def __init__(self, matchers, first_only=False):
        """Construct a MatchesListwise matcher.

        :param matchers: A list of matcher that the matched values must match.
        :param first_only: If True, then only report the first mismatch,
            otherwise report all of them. Defaults to False.
        """
        self.matchers = matchers
        self.first_only = first_only

    def match(self, values):
        from ._basic import Equals
        mismatches = []
        length_mismatch = Annotate(
            "Length mismatch", Equals(len(self.matchers))).match(len(values))
        if length_mismatch:
            mismatches.append(length_mismatch)
        for matcher, value in zip(self.matchers, values):
            mismatch = matcher.match(value)
            if mismatch:
                if self.first_only:
                    return mismatch
                mismatches.append(mismatch)
        if mismatches:
            return MismatchesAll(mismatches)


class MatchesStructure(object):
    """Matcher that matches an object structurally.

    'Structurally' here means that attributes of the object being matched are
    compared against given matchers.

    `fromExample` allows the creation of a matcher from a prototype object and
    then modified versions can be created with `update`.

    `byEquality` creates a matcher in much the same way as the constructor,
    except that the matcher for each of the attributes is assumed to be
    `Equals`.

    `byMatcher` creates a similar matcher to `byEquality`, but you get to pick
    the matcher, rather than just using `Equals`.
    """

    def __init__(self, **kwargs):
        """Construct a `MatchesStructure`.

        :param kwargs: A mapping of attributes to matchers.
        """
        self.kws = kwargs

    @classmethod
    def byEquality(cls, **kwargs):
        """Matches an object where the attributes equal the keyword values.

        Similar to the constructor, except that the matcher is assumed to be
        Equals.
        """
        from ._basic import Equals
        return cls.byMatcher(Equals, **kwargs)

    @classmethod
    def byMatcher(cls, matcher, **kwargs):
        """Matches an object where the attributes match the keyword values.

        Similar to the constructor, except that the provided matcher is used
        to match all of the values.
        """
        return cls(**map_values(matcher, kwargs))

    @classmethod
    def fromExample(cls, example, *attributes):
        from ._basic import Equals
        kwargs = {}
        for attr in attributes:
            kwargs[attr] = Equals(getattr(example, attr))
        return cls(**kwargs)

    def update(self, **kws):
        new_kws = self.kws.copy()
        for attr, matcher in kws.items():
            if matcher is None:
                new_kws.pop(attr, None)
            else:
                new_kws[attr] = matcher
        return type(self)(**new_kws)

    def __str__(self):
        kws = []
        for attr, matcher in sorted(self.kws.items()):
            kws.append("%s=%s" % (attr, matcher))
        return "%s(%s)" % (self.__class__.__name__, ', '.join(kws))

    def match(self, value):
        matchers = []
        values = []
        for attr, matcher in sorted(self.kws.items()):
            matchers.append(Annotate(attr, matcher))
            values.append(getattr(value, attr))
        return MatchesListwise(matchers).match(values)


class MatchesSetwise(object):
    """Matches if all the matchers match elements of the value being matched.

    That is, each element in the 'observed' set must match exactly one matcher
    from the set of matchers, with no matchers left over.

    The difference compared to `MatchesListwise` is that the order of the
    matchings does not matter.
    """

    def __init__(self, *matchers):
        self.matchers = matchers

    def match(self, observed):
        remaining_matchers = set(self.matchers)
        not_matched = []
        for value in observed:
            for matcher in remaining_matchers:
                if matcher.match(value) is None:
                    remaining_matchers.remove(matcher)
                    break
            else:
                not_matched.append(value)
        if not_matched or remaining_matchers:
            remaining_matchers = list(remaining_matchers)
            # There are various cases that all should be reported somewhat
            # differently.

            # There are two trivial cases:
            # 1) There are just some matchers left over.
            # 2) There are just some values left over.

            # Then there are three more interesting cases:
            # 3) There are the same number of matchers and values left over.
            # 4) There are more matchers left over than values.
            # 5) There are more values left over than matchers.

            if len(not_matched) == 0:
                if len(remaining_matchers) > 1:
                    msg = "There were %s matchers left over: " % (
                        len(remaining_matchers),)
                else:
                    msg = "There was 1 matcher left over: "
                msg += ', '.join(map(str, remaining_matchers))
                return Mismatch(msg)
            elif len(remaining_matchers) == 0:
                if len(not_matched) > 1:
                    return Mismatch(
                        "There were %s values left over: %s" % (
                            len(not_matched), not_matched))
                else:
                    return Mismatch(
                        "There was 1 value left over: %s" % (
                            not_matched, ))
            else:
                common_length = min(len(remaining_matchers), len(not_matched))
                if common_length == 0:
                    raise AssertionError("common_length can't be 0 here")
                if common_length > 1:
                    msg = "There were %s mismatches" % (common_length,)
                else:
                    msg = "There was 1 mismatch"
                if len(remaining_matchers) > len(not_matched):
                    extra_matchers = remaining_matchers[common_length:]
                    msg += " and %s extra matcher" % (len(extra_matchers), )
                    if len(extra_matchers) > 1:
                        msg += "s"
                    msg += ': ' + ', '.join(map(str, extra_matchers))
                elif len(not_matched) > len(remaining_matchers):
                    extra_values = not_matched[common_length:]
                    msg += " and %s extra value" % (len(extra_values), )
                    if len(extra_values) > 1:
                        msg += "s"
                    msg += ': ' + str(extra_values)
                return Annotate(
                    msg, MatchesListwise(remaining_matchers[:common_length])
                    ).match(not_matched[:common_length])
