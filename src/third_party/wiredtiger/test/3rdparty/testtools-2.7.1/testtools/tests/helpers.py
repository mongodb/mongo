# Copyright (c) 2008-2016 testtools developers. See LICENSE for details.

"""Helpers for tests."""

__all__ = [
    'LoggingResult',
    ]

import sys

from testtools import TestResult
from testtools.content import StackLinesContent
from testtools.matchers import (
    AfterPreprocessing,
    Equals,
    MatchesDict,
    MatchesListwise,
)
from testtools import runtest


# GZ 2010-08-12: Don't do this, pointlessly creates an exc_info cycle
try:
    raise Exception
except Exception:
    an_exc_info = sys.exc_info()


# Deprecated: This classes attributes are somewhat non deterministic which
# leads to hard to predict tests (because Python upstream are changing things.
class LoggingResult(TestResult):
    """TestResult that logs its event to a list."""

    def __init__(self, log):
        self._events = log
        super().__init__()

    def startTest(self, test):
        self._events.append(('startTest', test))
        super().startTest(test)

    def stop(self):
        self._events.append('stop')
        super().stop()

    def stopTest(self, test):
        self._events.append(('stopTest', test))
        super().stopTest(test)

    def addFailure(self, test, error):
        self._events.append(('addFailure', test, error))
        super().addFailure(test, error)

    def addError(self, test, error):
        self._events.append(('addError', test, error))
        super().addError(test, error)

    def addSkip(self, test, reason):
        self._events.append(('addSkip', test, reason))
        super().addSkip(test, reason)

    def addSuccess(self, test):
        self._events.append(('addSuccess', test))
        super().addSuccess(test)

    def startTestRun(self):
        self._events.append('startTestRun')
        super().startTestRun()

    def stopTestRun(self):
        self._events.append('stopTestRun')
        super().stopTestRun()

    def done(self):
        self._events.append('done')
        super().done()

    def tags(self, new_tags, gone_tags):
        self._events.append(('tags', new_tags, gone_tags))
        super().tags(new_tags, gone_tags)

    def time(self, a_datetime):
        self._events.append(('time', a_datetime))
        super().time(a_datetime)


def is_stack_hidden():
    return StackLinesContent.HIDE_INTERNAL_STACK


def hide_testtools_stack(should_hide=True):
    result = StackLinesContent.HIDE_INTERNAL_STACK
    StackLinesContent.HIDE_INTERNAL_STACK = should_hide
    return result


def run_with_stack_hidden(should_hide, f, *args, **kwargs):
    old_should_hide = hide_testtools_stack(should_hide)
    try:
        return f(*args, **kwargs)
    finally:
        hide_testtools_stack(old_should_hide)


class FullStackRunTest(runtest.RunTest):

    def _run_user(self, fn, *args, **kwargs):
        return run_with_stack_hidden(
            False,
            super()._run_user, fn, *args, **kwargs)


class MatchesEvents:
    """Match a list of test result events.

    Specify events as a data structure.  Ordinary Python objects within this
    structure will be compared exactly, but you can also use matchers at any
    point.
    """

    def __init__(self, *expected):
        self._expected = expected

    def _make_matcher(self, obj):
        # This isn't very safe for general use, but is good enough to make
        # some tests in this module more readable.
        if hasattr(obj, 'match'):
            return obj
        elif isinstance(obj, tuple) or isinstance(obj, list):
            return MatchesListwise(
                [self._make_matcher(item) for item in obj])
        elif isinstance(obj, dict):
            return MatchesDict({
                key: self._make_matcher(value)
                for key, value in obj.items()})
        else:
            return Equals(obj)

    def match(self, observed):
        matcher = self._make_matcher(self._expected)
        return matcher.match(observed)


class AsText(AfterPreprocessing):
    """Match the text of a Content instance."""

    def __init__(self, matcher, annotate=True):
        super().__init__(
            lambda log: log.as_text(), matcher, annotate=annotate)


def raise_(exception):
    """Raise ``exception``.

    Useful for raising exceptions when it is inconvenient to use a statement
    (e.g. in a lambda).

    :param Exception exception: An exception to raise.
    :raises: Whatever exception is

    """
    raise exception
