# Copyright (c) 2009-2012 testtools developers. See LICENSE for details.

__all__ = [
    'MatchesException',
    'Raises',
    'raises',
    ]

import sys

from testtools.compat import (
    classtypes,
    _error_repr,
    isbaseexception,
    istext,
    )
from ._basic import MatchesRegex
from ._higherorder import AfterPreproccessing
from ._impl import (
    Matcher,
    Mismatch,
    )


class MatchesException(Matcher):
    """Match an exc_info tuple against an exception instance or type."""

    def __init__(self, exception, value_re=None):
        """Create a MatchesException that will match exc_info's for exception.

        :param exception: Either an exception instance or type.
            If an instance is given, the type and arguments of the exception
            are checked. If a type is given only the type of the exception is
            checked. If a tuple is given, then as with isinstance, any of the
            types in the tuple matching is sufficient to match.
        :param value_re: If 'exception' is a type, and the matchee exception
            is of the right type, then match against this.  If value_re is a
            string, then assume value_re is a regular expression and match
            the str() of the exception against it.  Otherwise, assume value_re
            is a matcher, and match the exception against it.
        """
        Matcher.__init__(self)
        self.expected = exception
        if istext(value_re):
            value_re = AfterPreproccessing(str, MatchesRegex(value_re), False)
        self.value_re = value_re
        expected_type = type(self.expected)
        self._is_instance = not any(issubclass(expected_type, class_type) 
                for class_type in classtypes() + (tuple,))

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
            return self.value_re.match(other[1])

    def __str__(self):
        if self._is_instance:
            return "MatchesException(%s)" % _error_repr(self.expected)
        return "MatchesException(%s)" % repr(self.expected)


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
            exc_info = sys.exc_info()
            if self.exception_matcher:
                mismatch = self.exception_matcher.match(exc_info)
                if not mismatch:
                    del exc_info
                    return
            else:
                mismatch = None
            # The exception did not match, or no explicit matching logic was
            # performed. If the exception is a non-user exception (that is, not
            # a subclass of Exception on Python 2.5+) then propogate it.
            if isbaseexception(exc_info[1]):
                del exc_info
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
