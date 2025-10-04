# Copyright (c) testtools developers. See LICENSE for details.

"""Tests for Deferred matchers."""

from testtools.content import TracebackContent
from testtools.matchers import (
    AfterPreprocessing,
    Equals,
    Is,
    MatchesDict,
)
from ._helpers import NeedsTwistedTestCase


from testtools.twistedsupport import has_no_result, failed, succeeded

from twisted.internet import defer
from twisted.python.failure import Failure


def mismatches(description, details=None):
    """Match a ``Mismatch`` object."""
    if details is None:
        details = Equals({})

    matcher = MatchesDict({
        'description': description,
        'details': details,
    })

    def get_mismatch_info(mismatch):
        return {
            'description': mismatch.describe(),
            'details': mismatch.get_details(),
        }

    return AfterPreprocessing(get_mismatch_info, matcher)


def make_failure(exc_value):
    """Raise ``exc_value`` and return the failure."""
    try:
        raise exc_value
    except:
        return Failure()


class NoResultTests(NeedsTwistedTestCase):
    """Tests for ``has_no_result``."""

    def match(self, thing):
        return has_no_result().match(thing)

    def test_unfired_matches(self):
        # A Deferred that hasn't fired matches has_no_result().
        self.assertThat(self.match(defer.Deferred()), Is(None))

    def test_succeeded_does_no_match(self):
        # A Deferred that's fired successfully does not match has_no_result().
        result = object()
        deferred = defer.succeed(result)
        mismatch = self.match(deferred)
        self.assertThat(
            mismatch, mismatches(Equals(
                'No result expected on %r, found %r instead'
                % (deferred, result))))

    def test_failed_does_not_match(self):
        # A Deferred that's failed does not match has_no_result().
        fail = make_failure(RuntimeError('arbitrary failure'))
        deferred = defer.fail(fail)
        # Suppress unhandled error in Deferred.
        self.addCleanup(deferred.addErrback, lambda _: None)
        mismatch = self.match(deferred)
        self.assertThat(
            mismatch, mismatches(Equals(
                'No result expected on %r, found %r instead'
                % (deferred, fail))))

    def test_success_after_assertion(self):
        # We can create a Deferred, assert that it hasn't fired, then fire it
        # and collect the result.
        deferred = defer.Deferred()
        self.assertThat(deferred, has_no_result())
        results = []
        deferred.addCallback(results.append)
        marker = object()
        deferred.callback(marker)
        self.assertThat(results, Equals([marker]))

    def test_failure_after_assertion(self):
        # We can create a Deferred, assert that it hasn't fired, then fire it
        # with a failure and collect the result.
        deferred = defer.Deferred()
        self.assertThat(deferred, has_no_result())
        results = []
        deferred.addErrback(results.append)
        fail = make_failure(RuntimeError('arbitrary failure'))
        deferred.errback(fail)
        self.assertThat(results, Equals([fail]))


class SuccessResultTests(NeedsTwistedTestCase):

    def match(self, matcher, value):
        return succeeded(matcher).match(value)

    def test_succeeded_result_passes(self):
        # A Deferred that has fired successfully matches against the value it
        # was fired with.
        result = object()
        deferred = defer.succeed(result)
        self.assertThat(self.match(Is(result), deferred), Is(None))

    def test_different_succeeded_result_fails(self):
        # A Deferred that has fired successfully matches against the value it
        # was fired with.
        result = object()
        deferred = defer.succeed(result)
        matcher = Is(None)  # Something that doesn't match `result`.
        mismatch = matcher.match(result)
        self.assertThat(
            self.match(matcher, deferred),
            mismatches(Equals(mismatch.describe()),
                       Equals(mismatch.get_details())))

    def test_not_fired_fails(self):
        # A Deferred that has not yet fired fails to match.
        deferred = defer.Deferred()
        arbitrary_matcher = Is(None)
        self.assertThat(
            self.match(arbitrary_matcher, deferred),
            mismatches(
                Equals(('Success result expected on %r, found no result '
                        'instead') % (deferred,))))

    def test_failing_fails(self):
        # A Deferred that has fired with a failure fails to match.
        deferred = defer.Deferred()
        fail = make_failure(RuntimeError('arbitrary failure'))
        deferred.errback(fail)
        arbitrary_matcher = Is(None)
        self.assertThat(
            self.match(arbitrary_matcher, deferred),
            mismatches(
                Equals(
                    'Success result expected on %r, found failure result '
                     'instead: %r' % (deferred, fail)),
                Equals({'traceback': TracebackContent(
                    (fail.type, fail.value, fail.getTracebackObject()), None,
                )}),
            ))


class FailureResultTests(NeedsTwistedTestCase):

    def match(self, matcher, value):
        return failed(matcher).match(value)

    def test_failure_passes(self):
        # A Deferred that has fired with a failure matches against the value
        # it was fired with.
        fail = make_failure(RuntimeError('arbitrary failure'))
        deferred = defer.fail(fail)
        self.assertThat(self.match(Is(fail), deferred), Is(None))

    def test_different_failure_fails(self):
        # A Deferred that has fired with a failure matches against the value
        # it was fired with.
        fail = make_failure(RuntimeError('arbitrary failure'))
        deferred = defer.fail(fail)
        matcher = Is(None)  # Something that doesn't match `fail`.
        mismatch = matcher.match(fail)
        self.assertThat(
            self.match(matcher, deferred),
            mismatches(Equals(mismatch.describe()),
                       Equals(mismatch.get_details())))

    def test_success_fails(self):
        # A Deferred that has fired successfully fails to match.
        result = object()
        deferred = defer.succeed(result)
        matcher = Is(None)  # Can be any matcher
        self.assertThat(
            self.match(matcher, deferred),
            mismatches(Equals(
                'Failure result expected on %r, found success '
                'result (%r) instead' % (deferred, result))))

    def test_no_result_fails(self):
        # A Deferred that has not fired fails to match.
        deferred = defer.Deferred()
        matcher = Is(None)  # Can be any matcher
        self.assertThat(
            self.match(matcher, deferred),
            mismatches(Equals(
                'Failure result expected on %r, found no result instead'
                % (deferred,))))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
