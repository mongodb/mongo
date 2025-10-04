# Copyright (c) testtools developers. See LICENSE for details.

"""Matchers that operate on synchronous Deferreds.

A "synchronous" Deferred is one that does not need the reactor or any other
asynchronous process in order to fire.

Normal application code can't know when a Deferred is going to fire, because
that is generally left up to the reactor. Unit tests can (and should!) provide
fake reactors, or don't use the reactor at all, so that Deferreds fire
synchronously.

These matchers allow you to make assertions about when and how Deferreds fire,
and about what values they fire with.
"""

from testtools.matchers import Mismatch

from ._deferred import failure_content, on_deferred_result


class _NoResult:
    """Matches a Deferred that has not yet fired."""

    @staticmethod
    def _got_result(deferred, result):
        return Mismatch(
            'No result expected on %r, found %r instead'
            % (deferred, result))

    def match(self, deferred):
        """Match ``deferred`` if it hasn't fired."""
        return on_deferred_result(
            deferred,
            on_success=self._got_result,
            on_failure=self._got_result,
            on_no_result=lambda _: None,
        )


_NO_RESULT = _NoResult()


def has_no_result():
    """Match a Deferred that has not yet fired.

    For example, this will pass::

        assert_that(defer.Deferred(), has_no_result())

    But this will fail:

    >>> assert_that(defer.succeed(None), has_no_result())
    Traceback (most recent call last):
      ...
      File "testtools/assertions.py", line 22, in assert_that
        raise MismatchError(matchee, matcher, mismatch, verbose)
    testtools.matchers._impl.MismatchError: No result expected on <Deferred at ... current result: None>, found None instead

    As will this:

    >>> assert_that(defer.fail(RuntimeError('foo')), has_no_result())
    Traceback (most recent call last):
      ...
      File "testtools/assertions.py", line 22, in assert_that
        raise MismatchError(matchee, matcher, mismatch, verbose)
    testtools.matchers._impl.MismatchError: No result expected on <Deferred at ... current result: <twisted.python.failure.Failure <type 'exceptions.RuntimeError'>>>, found <twisted.python.failure.Failure <type 'exceptions.RuntimeError'>> instead
    """
    return _NO_RESULT


class _Succeeded:
    """Matches a Deferred that has fired successfully."""

    def __init__(self, matcher):
        """Construct a ``_Succeeded`` matcher."""
        self._matcher = matcher

    @staticmethod
    def _got_failure(deferred, failure):
        deferred.addErrback(lambda _: None)
        return Mismatch(
            'Success result expected on %r, found failure result '
            'instead: %r' % (deferred, failure),
            {'traceback': failure_content(failure)},
        )

    @staticmethod
    def _got_no_result(deferred):
        return Mismatch(
            'Success result expected on %r, found no result '
            'instead' % (deferred,))

    def match(self, deferred):
        """Match against the successful result of ``deferred``."""
        return on_deferred_result(
            deferred,
            on_success=lambda _, value: self._matcher.match(value),
            on_failure=self._got_failure,
            on_no_result=self._got_no_result,
        )


def succeeded(matcher):
    """Match a Deferred that has fired successfully.

    For example::

        fires_with_the_answer = succeeded(Equals(42))
        deferred = defer.succeed(42)
        assert_that(deferred, fires_with_the_answer)

    This assertion will pass. However, if ``deferred`` had fired with a
    different value, or had failed, or had not fired at all, then it would
    fail.

    Use this instead of
    :py:meth:`twisted.trial.unittest.SynchronousTestCase.successResultOf`.

    :param matcher: A matcher to match against the result of a
        :class:`~twisted.internet.defer.Deferred`.
    :return: A matcher that can be applied to a synchronous
        :class:`~twisted.internet.defer.Deferred`.
    """
    return _Succeeded(matcher)


class _Failed:
    """Matches a Deferred that has failed."""

    def __init__(self, matcher):
        self._matcher = matcher

    def _got_failure(self, deferred, failure):
        # We have handled the failure, so suppress its output.
        deferred.addErrback(lambda _: None)
        return self._matcher.match(failure)

    @staticmethod
    def _got_success(deferred, success):
        return Mismatch(
            'Failure result expected on %r, found success '
            'result (%r) instead' % (deferred, success))

    @staticmethod
    def _got_no_result(deferred):
        return Mismatch(
            'Failure result expected on %r, found no result instead'
            % (deferred,))

    def match(self, deferred):
        return on_deferred_result(
            deferred,
            on_success=self._got_success,
            on_failure=self._got_failure,
            on_no_result=self._got_no_result,
        )


def failed(matcher):
    """Match a Deferred that has failed.

    For example::

        error = RuntimeError('foo')
        fails_at_runtime = failed(
            AfterPreprocessing(lambda f: f.value, Equals(error)))
        deferred = defer.fail(error)
        assert_that(deferred, fails_at_runtime)

    This assertion will pass. However, if ``deferred`` had fired successfully,
    had failed with a different error, or had not fired at all, then it would
    fail.

    Use this instead of
    :py:meth:`twisted.trial.unittest.SynchronousTestCase.failureResultOf`.

    :param matcher: A matcher to match against the result of a failing
        :class:`~twisted.internet.defer.Deferred`.
    :return: A matcher that can be applied to a synchronous
        :class:`~twisted.internet.defer.Deferred`.
    """
    return _Failed(matcher)
