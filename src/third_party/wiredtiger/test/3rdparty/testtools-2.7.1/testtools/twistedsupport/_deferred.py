# Copyright (c) testtools developers. See LICENSE for details.

"""Utilities for Deferreds."""

from typing import List, TYPE_CHECKING

from functools import partial

if TYPE_CHECKING:
    from twisted.python.failure import Failure
    from twisted.internet.defer import Deferred

from testtools.content import TracebackContent


class DeferredNotFired(Exception):
    """Raised when we extract a result from a Deferred that's not fired yet."""

    def __init__(self, deferred):
        msg = f"{deferred!r} has not fired yet."
        super().__init__(msg)


def extract_result(deferred):
    """Extract the result from a fired deferred.

    It can happen that you have an API that returns Deferreds for
    compatibility with Twisted code, but is in fact synchronous, i.e. the
    Deferreds it returns have always fired by the time it returns.  In this
    case, you can use this function to convert the result back into the usual
    form for a synchronous API, i.e. the result itself or a raised exception.

    As a rule, this function should not be used when operating with
    asynchronous Deferreds (i.e. for normal use of Deferreds in application
    code). In those cases, it is better to add callbacks and errbacks as
    needed.
    """
    failures: List["Failure"] = []
    successes: List["Deferred"] = []
    deferred.addCallbacks(successes.append, failures.append)
    if len(failures) == 1:
        failures[0].raiseException()
    elif len(successes) == 1:
        return successes[0]
    else:
        raise DeferredNotFired(deferred)


class ImpossibleDeferredError(Exception):
    """Raised if a Deferred somehow triggers both a success and a failure."""

    def __init__(self, deferred, successes, failures):
        msg = ('Impossible condition on %r, got both success (%r) and '
               'failure (%r)')
        super().__init__(
            msg % (deferred, successes, failures))


def on_deferred_result(deferred, on_success, on_failure, on_no_result):
    """Handle the result of a synchronous ``Deferred``.

    If ``deferred`` has fire successfully, call ``on_success``.
    If ``deferred`` has failed, call ``on_failure``.
    If ``deferred`` has not yet fired, call ``on_no_result``.

    The value of ``deferred`` will be preserved, so that other callbacks and
    errbacks can be added to ``deferred``.

    :param Deferred[A] deferred: A synchronous Deferred.
    :param Callable[[Deferred[A], A], T] on_success: Called if the Deferred
        fires successfully.
    :param Callable[[Deferred[A], Failure], T] on_failure: Called if the
        Deferred fires unsuccessfully.
    :param Callable[[Deferred[A]], T] on_no_result: Called if the Deferred has
        not yet fired.

    :raises ImpossibleDeferredError: If the Deferred somehow
        triggers both a success and a failure.
    :raises TypeError: If the Deferred somehow triggers more than one success,
        or more than one failure.

    :return: Whatever is returned by the triggered callback.
    :rtype: ``T``
    """
    successes: List["Deferred"] = []
    failures: List["Failure"] = []

    def capture(value, values):
        values.append(value)
        return value

    deferred.addCallbacks(
        partial(capture, values=successes),
        partial(capture, values=failures),
    )

    if successes and failures:
        raise ImpossibleDeferredError(deferred, successes, failures)
    elif failures:
        [failure] = failures
        return on_failure(deferred, failure)
    elif successes:
        [result] = successes
        return on_success(deferred, result)
    else:
        return on_no_result(deferred)


def failure_content(failure):
    """Create a Content object for a Failure.

    :param Failure failure: The failure to create content for.
    :rtype: ``Content``
    """
    return TracebackContent(
        (failure.type, failure.value, failure.getTracebackObject()),
        None,
    )
