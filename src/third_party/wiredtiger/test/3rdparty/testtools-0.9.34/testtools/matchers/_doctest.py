# Copyright (c) 2009-2012 testtools developers. See LICENSE for details.

__all__ = [
    'DocTestMatches',
    ]

import doctest
import re

from ..compat import str_is_unicode
from ._impl import Mismatch


class _NonManglingOutputChecker(doctest.OutputChecker):
    """Doctest checker that works with unicode rather than mangling strings

    This is needed because current Python versions have tried to fix string
    encoding related problems, but regressed the default behaviour with
    unicode inputs in the process.

    In Python 2.6 and 2.7 ``OutputChecker.output_difference`` is was changed
    to return a bytestring encoded as per ``sys.stdout.encoding``, or utf-8 if
    that can't be determined. Worse, that encoding process happens in the
    innocent looking `_indent` global function. Because the
    `DocTestMismatch.describe` result may well not be destined for printing to
    stdout, this is no good for us. To get a unicode return as before, the
    method is monkey patched if ``doctest._encoding`` exists.

    Python 3 has a different problem. For some reason both inputs are encoded
    to ascii with 'backslashreplace', making an escaped string matches its
    unescaped form. Overriding the offending ``OutputChecker._toAscii`` method
    is sufficient to revert this.
    """

    def _toAscii(self, s):
        """Return ``s`` unchanged rather than mangling it to ascii"""
        return s

    # Only do this overriding hackery if doctest has a broken _input function
    if getattr(doctest, "_encoding", None) is not None:
        from types import FunctionType as __F
        __f = doctest.OutputChecker.output_difference.im_func
        __g = dict(__f.func_globals)
        def _indent(s, indent=4, _pattern=re.compile("^(?!$)", re.MULTILINE)):
            """Prepend non-empty lines in ``s`` with ``indent`` number of spaces"""
            return _pattern.sub(indent*" ", s)
        __g["_indent"] = _indent
        output_difference = __F(__f.func_code, __g, "output_difference")
        del __F, __f, __g, _indent


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
        self._checker = _NonManglingOutputChecker()

    def __str__(self):
        if self.flags:
            flagstr = ", flags=%d" % self.flags
        else:
            flagstr = ""
        return 'DocTestMatches(%r%s)' % (self.want, flagstr)

    def _with_nl(self, actual):
        result = self.want.__class__(actual)
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
        s = self.matcher._describe_difference(self.with_nl)
        if str_is_unicode or isinstance(s, unicode):
            return s
        # GZ 2011-08-24: This is actually pretty bogus, most C0 codes should
        #                be escaped, in addition to non-ascii bytes.
        return s.decode("latin1").encode("ascii", "backslashreplace")
