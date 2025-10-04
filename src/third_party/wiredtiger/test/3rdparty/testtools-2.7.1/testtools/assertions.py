# Copyright (c) 2008-2017 testtools developers. See LICENSE for details.

"""Assertion helpers."""

from testtools.matchers import (
    Annotate,
    MismatchError,
    )


def assert_that(matchee, matcher, message='', verbose=False):
    """Assert that matchee is matched by matcher.

    This should only be used when you need to use a function based
    matcher, assertThat in Testtools.Testcase is preferred and has more
    features

    :param matchee: An object to match with matcher.
    :param matcher: An object meeting the testtools.Matcher protocol.
    :raises MismatchError: When matcher does not match thing.
    """
    matcher = Annotate.if_message(message, matcher)
    mismatch = matcher.match(matchee)
    if not mismatch:
        return
    raise MismatchError(matchee, matcher, mismatch, verbose)
