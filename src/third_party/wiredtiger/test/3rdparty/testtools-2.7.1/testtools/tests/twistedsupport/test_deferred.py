# Copyright (c) testtools developers. See LICENSE for details.

"""Tests for testtools._deferred."""

from testtools.helpers import try_import
from testtools.matchers import (
    Equals,
    MatchesException,
    Raises,
    )
from ._helpers import NeedsTwistedTestCase


DeferredNotFired = try_import(
    'testtools.twistedsupport._deferred.DeferredNotFired')
extract_result = try_import(
    'testtools.twistedsupport._deferred.extract_result')

defer = try_import('twisted.internet.defer')
Failure = try_import('twisted.python.failure.Failure')


class TestExtractResult(NeedsTwistedTestCase):
    """Tests for ``extract_result``."""

    def test_not_fired(self):
        # _spinner.extract_result raises _spinner.DeferredNotFired if it's
        # given a Deferred that has not fired.
        self.assertThat(
            lambda: extract_result(defer.Deferred()),
            Raises(MatchesException(DeferredNotFired)))

    def test_success(self):
        # _spinner.extract_result returns the value of the Deferred if it has
        # fired successfully.
        marker = object()
        d = defer.succeed(marker)
        self.assertThat(extract_result(d), Equals(marker))

    def test_failure(self):
        # _spinner.extract_result raises the failure's exception if it's given
        # a Deferred that is failing.
        try:
            1/0
        except ZeroDivisionError:
            f = Failure()
        d = defer.fail(f)
        self.assertThat(
            lambda: extract_result(d),
            Raises(MatchesException(ZeroDivisionError)))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
