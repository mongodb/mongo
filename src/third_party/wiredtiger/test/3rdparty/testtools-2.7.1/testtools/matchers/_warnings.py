# Copyright (c) 2009-2016 testtools developers. See LICENSE for details.

__all__ = [
    'Warnings',
    'WarningMessage',
    'IsDeprecated']

import warnings

from ._basic import Is
from ._const import Always
from ._datastructures import MatchesListwise, MatchesStructure
from ._higherorder import (
    AfterPreprocessing,
    Annotate,
    )
from ._impl import Mismatch


def WarningMessage(category_type, message=None, filename=None, lineno=None,
                   line=None):
    r"""
    Create a matcher that will match `warnings.WarningMessage`\s.

    For example, to match captured `DeprecationWarning`s with a message about
    some ``foo`` being replaced with ``bar``:

    .. code-block:: python

       WarningMessage(DeprecationWarning,
                      message=MatchesAll(
                          Contains('foo is deprecated'),
                          Contains('use bar instead')))

    :param type category_type: A warning type, for example `DeprecationWarning`.
    :param message_matcher: A matcher object that will be evaluated against
        warning's message.
    :param filename_matcher: A matcher object that will be evaluated against
        the warning's filename.
    :param lineno_matcher: A matcher object that will be evaluated against the
        warning's line number.
    :param line_matcher: A matcher object that will be evaluated against the
        warning's line of source code.
    """
    category_matcher = Is(category_type)
    message_matcher = message or Always()
    filename_matcher = filename or Always()
    lineno_matcher = lineno or Always()
    line_matcher = line or Always()
    return MatchesStructure(
        category=Annotate(
            "Warning's category type does not match",
            category_matcher),
        message=Annotate(
            "Warning's message does not match",
            AfterPreprocessing(str, message_matcher)),
        filename=Annotate(
            "Warning's filname does not match",
            filename_matcher),
        lineno=Annotate(
            "Warning's line number does not match",
            lineno_matcher),
        line=Annotate(
            "Warning's source line does not match",
            line_matcher))


class Warnings:
    """
    Match if the matchee produces warnings.
    """
    def __init__(self, warnings_matcher=None):
        """
        Create a Warnings matcher.

        :param warnings_matcher: Optional validator for the warnings emitted by
        matchee. If no warnings_matcher is supplied then the simple fact that
        at least one warning is emitted is considered enough to match on.
        """
        self.warnings_matcher = warnings_matcher

    def match(self, matchee):
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter('always')
            matchee()
            if self.warnings_matcher is not None:
                return self.warnings_matcher.match(w)
            elif not w:
                return Mismatch('Expected at least one warning, got none')

    def __str__(self):
        return f'Warnings({self.warnings_matcher!s})'


def IsDeprecated(message):
    """
    Make a matcher that checks that a callable produces exactly one
    `DeprecationWarning`.

    :param message: Matcher for the warning message.
    """
    return Warnings(
        MatchesListwise([
            WarningMessage(
                category_type=DeprecationWarning,
                message=message)]))
