# Copyright (c) 2009-2011 testtools developers. See LICENSE for details.

"""Matchers, a way to express complex assertions outside the testcase.

Inspired by 'hamcrest'.

Matcher provides the abstract API that all matchers need to implement.

Bundled matchers are listed in __all__: a list can be obtained by running
$ python -c 'import testtools.matchers; print testtools.matchers.__all__'
"""

__metaclass__ = type
__all__ = [
    'Annotate',
    'DocTestMatches',
    'EndsWith',
    'Equals',
    'Is',
    'KeysEqual',
    'LessThan',
    'MatchesAll',
    'MatchesAny',
    'MatchesException',
    'NotEquals',
    'Not',
    'Raises',
    'raises',
    'StartsWith',
    ]

import doctest
import operator
from pprint import pformat
import re
import sys
import types

from testtools.compat import classtypes, _error_repr, isbaseexception


class Matcher(object):
    """A pattern matcher.

    A Matcher must implement match and __str__ to be used by
    testtools.TestCase.assertThat. Matcher.match(thing) returns None when
    thing is completely matched, and a Mismatch object otherwise.

    Matchers can be useful outside of test cases, as they are simply a
    pattern matching language expressed as objects.

    testtools.matchers is inspired by hamcrest, but is pythonic rather than
    a Java transcription.
    """

    def match(self, something):
        """Return None if this matcher matches something, a Mismatch otherwise.
        """
        raise NotImplementedError(self.match)

    def __str__(self):
        """Get a sensible human representation of the matcher.

        This should include the parameters given to the matcher and any
        state that would affect the matches operation.
        """
        raise NotImplementedError(self.__str__)


class Mismatch(object):
    """An object describing a mismatch detected by a Matcher."""

    def __init__(self, description=None, details=None):
        """Construct a `Mismatch`.

        :param description: A description to use.  If not provided,
            `Mismatch.describe` must be implemented.
        :param details: Extra details about the mismatch.  Defaults
            to the empty dict.
        """
        if description:
            self._description = description
        if details is None:
            details = {}
        self._details = details

    def describe(self):
        """Describe the mismatch.

        This should be either a human-readable string or castable to a string.
        """
        try:
            return self._description
        except AttributeError:
            raise NotImplementedError(self.describe)

    def get_details(self):
        """Get extra details about the mismatch.

        This allows the mismatch to provide extra information beyond the basic
        description, including large text or binary files, or debugging internals
        without having to force it to fit in the output of 'describe'.

        The testtools assertion assertThat will query get_details and attach
        all its values to the test, permitting them to be reported in whatever
        manner the test environment chooses.

        :return: a dict mapping names to Content objects. name is a string to
            name the detail, and the Content object is the detail to add
            to the result. For more information see the API to which items from
            this dict are passed testtools.TestCase.addDetail.
        """
        return getattr(self, '_details', {})

    def __repr__(self):
        return  "<testtools.matchers.Mismatch object at %x attributes=%r>" % (
            id(self), self.__dict__)


class MismatchDecorator(object):
    """Decorate a ``Mismatch``.

    Forwards all messages to the original mismatch object.  Probably the best
    way to use this is inherit from this class and then provide your own
    custom decoration logic.
    """

    def __init__(self, original):
        """Construct a `MismatchDecorator`.

        :param original: A `Mismatch` object to decorate.
        """
        self.original = original

    def __repr__(self):
        return '<testtools.matchers.MismatchDecorator(%r)>' % (self.original,)

    def describe(self):
        return self.original.describe()

    def get_details(self):
        return self.original.get_details()


class DocTestMatches(object):
    """See if a string matches a doctest example."""

    def __init__(self, example, flags=0):
        """Create a DocTestMatches to match example.

        :param example: The example to match e.g. 'foo bar baz'
        :param flags: doctest comparison flags to match on. e.g.
            doctest.ELLIPSIS.
        """
        if not example.endswith('\n'):
            example += '\n'
        self.want = example # required variable name by doctest.
        self.flags = flags
        self._checker = doctest.OutputChecker()

    def __str__(self):
        if self.flags:
            flagstr = ", flags=%d" % self.flags
        else:
            flagstr = ""
        return 'DocTestMatches(%r%s)' % (self.want, flagstr)

    def _with_nl(self, actual):
        result = str(actual)
        if not result.endswith('\n'):
            result += '\n'
        return result

    def match(self, actual):
        with_nl = self._with_nl(actual)
        if self._checker.check_output(self.want, with_nl, self.flags):
            return None
        return DocTestMismatch(self, with_nl)

    def _describe_difference(self, with_nl):
        return self._checker.output_difference(self, with_nl, self.flags)


class DocTestMismatch(Mismatch):
    """Mismatch object for DocTestMatches."""

    def __init__(self, matcher, with_nl):
        self.matcher = matcher
        self.with_nl = with_nl

    def describe(self):
        return self.matcher._describe_difference(self.with_nl)


class DoesNotStartWith(Mismatch):

    def __init__(self, matchee, expected):
        """Create a DoesNotStartWith Mismatch.

        :param matchee: the string that did not match.
        :param expected: the string that 'matchee' was expected to start with.
        """
        self.matchee = matchee
        self.expected = expected

    def describe(self):
        return "'%s' does not start with '%s'." % (
            self.matchee, self.expected)


class DoesNotEndWith(Mismatch):

    def __init__(self, matchee, expected):
        """Create a DoesNotEndWith Mismatch.

        :param matchee: the string that did not match.
        :param expected: the string that 'matchee' was expected to end with.
        """
        self.matchee = matchee
        self.expected = expected

    def describe(self):
        return "'%s' does not end with '%s'." % (
            self.matchee, self.expected)


class _BinaryComparison(object):
    """Matcher that compares an object to another object."""

    def __init__(self, expected):
        self.expected = expected

    def __str__(self):
        return "%s(%r)" % (self.__class__.__name__, self.expected)

    def match(self, other):
        if self.comparator(other, self.expected):
            return None
        return _BinaryMismatch(self.expected, self.mismatch_string, other)

    def comparator(self, expected, other):
        raise NotImplementedError(self.comparator)


class _BinaryMismatch(Mismatch):
    """Two things did not match."""

    def __init__(self, expected, mismatch_string, other):
        self.expected = expected
        self._mismatch_string = mismatch_string
        self.other = other

    def describe(self):
        left = repr(self.expected)
        right = repr(self.other)
        if len(left) + len(right) > 70:
            return "%s:\nreference = %s\nactual = %s\n" % (
                self._mismatch_string, pformat(self.expected),
                pformat(self.other))
        else:
            return "%s %s %s" % (left, self._mismatch_string,right)


class Equals(_BinaryComparison):
    """Matches if the items are equal."""

    comparator = operator.eq
    mismatch_string = '!='


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
    mismatch_string = 'is not >'


class MatchesAny(object):
    """Matches if any of the matchers it is created with match."""

    def __init__(self, *matchers):
        self.matchers = matchers

    def match(self, matchee):
        results = []
        for matcher in self.matchers:
            mismatch = matcher.match(matchee)
            if mismatch is None:
                return None
            results.append(mismatch)
        return MismatchesAll(results)

    def __str__(self):
        return "MatchesAny(%s)" % ', '.join([
            str(matcher) for matcher in self.matchers])


class MatchesAll(object):
    """Matches if all of the matchers it is created with match."""

    def __init__(self, *matchers):
        self.matchers = matchers

    def __str__(self):
        return 'MatchesAll(%s)' % ', '.join(map(str, self.matchers))

    def match(self, matchee):
        results = []
        for matcher in self.matchers:
            mismatch = matcher.match(matchee)
            if mismatch is not None:
                results.append(mismatch)
        if results:
            return MismatchesAll(results)
        else:
            return None


class MismatchesAll(Mismatch):
    """A mismatch with many child mismatches."""

    def __init__(self, mismatches):
        self.mismatches = mismatches

    def describe(self):
        descriptions = ["Differences: ["]
        for mismatch in self.mismatches:
            descriptions.append(mismatch.describe())
        descriptions.append("]")
        return '\n'.join(descriptions)


class Not(object):
    """Inverts a matcher."""

    def __init__(self, matcher):
        self.matcher = matcher

    def __str__(self):
        return 'Not(%s)' % (self.matcher,)

    def match(self, other):
        mismatch = self.matcher.match(other)
        if mismatch is None:
            return MatchedUnexpectedly(self.matcher, other)
        else:
            return None


class MatchedUnexpectedly(Mismatch):
    """A thing matched when it wasn't supposed to."""

    def __init__(self, matcher, other):
        self.matcher = matcher
        self.other = other

    def describe(self):
        return "%r matches %s" % (self.other, self.matcher)


class MatchesException(Matcher):
    """Match an exc_info tuple against an exception instance or type."""

    def __init__(self, exception, value_re=None):
        """Create a MatchesException that will match exc_info's for exception.

        :param exception: Either an exception instance or type.
            If an instance is given, the type and arguments of the exception
            are checked. If a type is given only the type of the exception is
            checked.
        :param value_re: If 'exception' is a type, and the matchee exception
            is of the right type, then the 'str()' of the matchee exception
            is matched against this regular expression.
        """
        Matcher.__init__(self)
        self.expected = exception
        self.value_re = value_re
        self._is_instance = type(self.expected) not in classtypes()

    def match(self, other):
        if type(other) != tuple:
            return Mismatch('%r is not an exc_info tuple' % other)
        expected_class = self.expected
        if self._is_instance:
            expected_class = expected_class.__class__
        if not issubclass(other[0], expected_class):
            return Mismatch('%r is not a %r' % (other[0], expected_class))
        if self._is_instance:
            if other[1].args != self.expected.args:
                return Mismatch('%s has different arguments to %s.' % (
                        _error_repr(other[1]), _error_repr(self.expected)))
        elif self.value_re is not None:
            str_exc_value = str(other[1])
            if not re.match(self.value_re, str_exc_value):
                return Mismatch(
                    '"%s" does not match "%s".'
                    % (str_exc_value, self.value_re))

    def __str__(self):
        if self._is_instance:
            return "MatchesException(%s)" % _error_repr(self.expected)
        return "MatchesException(%s)" % repr(self.expected)


class StartsWith(Matcher):
    """Checks whether one string starts with another."""

    def __init__(self, expected):
        """Create a StartsWith Matcher.

        :param expected: the string that matchees should start with.
        """
        self.expected = expected

    def __str__(self):
        return "Starts with '%s'." % self.expected

    def match(self, matchee):
        if not matchee.startswith(self.expected):
            return DoesNotStartWith(matchee, self.expected)
        return None


class EndsWith(Matcher):
    """Checks whether one string starts with another."""

    def __init__(self, expected):
        """Create a EndsWith Matcher.

        :param expected: the string that matchees should end with.
        """
        self.expected = expected

    def __str__(self):
        return "Ends with '%s'." % self.expected

    def match(self, matchee):
        if not matchee.endswith(self.expected):
            return DoesNotEndWith(matchee, self.expected)
        return None


class KeysEqual(Matcher):
    """Checks whether a dict has particular keys."""

    def __init__(self, *expected):
        """Create a `KeysEqual` Matcher.

        :param expected: The keys the dict is expected to have.  If a dict,
            then we use the keys of that dict, if a collection, we assume it
            is a collection of expected keys.
        """
        try:
            self.expected = expected.keys()
        except AttributeError:
            self.expected = list(expected)

    def __str__(self):
        return "KeysEqual(%s)" % ', '.join(map(repr, self.expected))

    def match(self, matchee):
        expected = sorted(self.expected)
        matched = Equals(expected).match(sorted(matchee.keys()))
        if matched:
            return AnnotatedMismatch(
                'Keys not equal',
                _BinaryMismatch(expected, 'does not match', matchee))
        return None


class Annotate(object):
    """Annotates a matcher with a descriptive string.

    Mismatches are then described as '<mismatch>: <annotation>'.
    """

    def __init__(self, annotation, matcher):
        self.annotation = annotation
        self.matcher = matcher

    def __str__(self):
        return 'Annotate(%r, %s)' % (self.annotation, self.matcher)

    def match(self, other):
        mismatch = self.matcher.match(other)
        if mismatch is not None:
            return AnnotatedMismatch(self.annotation, mismatch)


class AnnotatedMismatch(MismatchDecorator):
    """A mismatch annotated with a descriptive string."""

    def __init__(self, annotation, mismatch):
        super(AnnotatedMismatch, self).__init__(mismatch)
        self.annotation = annotation
        self.mismatch = mismatch

    def describe(self):
        return '%s: %s' % (self.original.describe(), self.annotation)


class Raises(Matcher):
    """Match if the matchee raises an exception when called.

    Exceptions which are not subclasses of Exception propogate out of the
    Raises.match call unless they are explicitly matched.
    """

    def __init__(self, exception_matcher=None):
        """Create a Raises matcher.

        :param exception_matcher: Optional validator for the exception raised
            by matchee. If supplied the exc_info tuple for the exception raised
            is passed into that matcher. If no exception_matcher is supplied
            then the simple fact of raising an exception is considered enough
            to match on.
        """
        self.exception_matcher = exception_matcher

    def match(self, matchee):
        try:
            result = matchee()
            return Mismatch('%r returned %r' % (matchee, result))
        # Catch all exceptions: Raises() should be able to match a
        # KeyboardInterrupt or SystemExit.
        except:
            if self.exception_matcher:
                mismatch = self.exception_matcher.match(sys.exc_info())
                if not mismatch:
                    return
            else:
                mismatch = None
            # The exception did not match, or no explicit matching logic was
            # performed. If the exception is a non-user exception (that is, not
            # a subclass of Exception on Python 2.5+) then propogate it.
            if isbaseexception(sys.exc_info()[1]):
                raise
            return mismatch

    def __str__(self):
        return 'Raises()'


def raises(exception):
    """Make a matcher that checks that a callable raises an exception.

    This is a convenience function, exactly equivalent to::

        return Raises(MatchesException(exception))

    See `Raises` and `MatchesException` for more information.
    """
    return Raises(MatchesException(exception))


class MatchesListwise(object):
    """Matches if each matcher matches the corresponding value.

    More easily explained by example than in words:

    >>> MatchesListwise([Equals(1)]).match([1])
    >>> MatchesListwise([Equals(1), Equals(2)]).match([1, 2])
    >>> print (MatchesListwise([Equals(1), Equals(2)]).match([2, 1]).describe())
    Differences: [
    1 != 2
    2 != 1
    ]
    """

    def __init__(self, matchers):
        self.matchers = matchers

    def match(self, values):
        mismatches = []
        length_mismatch = Annotate(
            "Length mismatch", Equals(len(self.matchers))).match(len(values))
        if length_mismatch:
            mismatches.append(length_mismatch)
        for matcher, value in zip(self.matchers, values):
            mismatch = matcher.match(value)
            if mismatch:
                mismatches.append(mismatch)
        if mismatches:
            return MismatchesAll(mismatches)


class MatchesStructure(object):
    """Matcher that matches an object structurally.

    'Structurally' here means that attributes of the object being matched are
    compared against given matchers.

    `fromExample` allows the creation of a matcher from a prototype object and
    then modified versions can be created with `update`.
    """

    def __init__(self, **kwargs):
        """Construct a `MatchesStructure`.

        :param kwargs: A mapping of attributes to matchers.
        """
        self.kws = kwargs

    @classmethod
    def fromExample(cls, example, *attributes):
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


class MatchesRegex(object):
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
        return '%s(%s)' % (self.__class__.__name__, ', '.join(args))

    def match(self, value):
        if not re.match(self.pattern, value, self.flags):
            return Mismatch("%r did not match %r" % (self.pattern, value))


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


class AfterPreproccessing(object):
    """Matches if the value matches after passing through a function.

    This can be used to aid in creating trivial matchers as functions, for
    example::

      def PathHasFileContent(content):
          def _read(path):
              return open(path).read()
          return AfterPreproccessing(_read, Equals(content))
    """

    def __init__(self, preprocessor, matcher):
        self.preprocessor = preprocessor
        self.matcher = matcher

    def _str_preprocessor(self):
        if isinstance(self.preprocessor, types.FunctionType):
            return '<function %s>' % self.preprocessor.__name__
        return str(self.preprocessor)

    def __str__(self):
        return "AfterPreproccessing(%s, %s)" % (
            self._str_preprocessor(), self.matcher)

    def match(self, value):
        value = self.preprocessor(value)
        return Annotate(
            "after %s" % self._str_preprocessor(),
            self.matcher).match(value)
