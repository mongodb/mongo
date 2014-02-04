# Copyright (c) 2010 testtools developers. See LICENSE for details.

"""Evil reactor-spinning logic for running Twisted tests.

This code is highly experimental, liable to change and not to be trusted.  If
you couldn't write this yourself, you should not be using it.
"""

__all__ = [
    'DeferredNotFired',
    'extract_result',
    'NoResultError',
    'not_reentrant',
    'ReentryError',
    'Spinner',
    'StaleJunkError',
    'TimeoutError',
    'trap_unhandled_errors',
    ]

import signal

from testtools.monkey import MonkeyPatcher

from twisted.internet import defer
from twisted.internet.base import DelayedCall
from twisted.internet.interfaces import IReactorThreads
from twisted.python.failure import Failure
from twisted.python.util import mergeFunctionMetadata


class ReentryError(Exception):
    """Raised when we try to re-enter a function that forbids it."""

    def __init__(self, function):
        Exception.__init__(self,
            "%r in not re-entrant but was called within a call to itself."
            % (function,))


def not_reentrant(function, _calls={}):
    """Decorates a function as not being re-entrant.

    The decorated function will raise an error if called from within itself.
    """
    def decorated(*args, **kwargs):
        if _calls.get(function, False):
            raise ReentryError(function)
        _calls[function] = True
        try:
            return function(*args, **kwargs)
        finally:
            _calls[function] = False
    return mergeFunctionMetadata(function, decorated)


class DeferredNotFired(Exception):
    """Raised when we extract a result from a Deferred that's not fired yet."""


def extract_result(deferred):
    """Extract the result from a fired deferred.

    It can happen that you have an API that returns Deferreds for
    compatibility with Twisted code, but is in fact synchronous, i.e. the
    Deferreds it returns have always fired by the time it returns.  In this
    case, you can use this function to convert the result back into the usual
    form for a synchronous API, i.e. the result itself or a raised exception.

    It would be very bad form to use this as some way of checking if a
    Deferred has fired.
    """
    failures = []
    successes = []
    deferred.addCallbacks(successes.append, failures.append)
    if len(failures) == 1:
        failures[0].raiseException()
    elif len(successes) == 1:
        return successes[0]
    else:
        raise DeferredNotFired("%r has not fired yet." % (deferred,))


def trap_unhandled_errors(function, *args, **kwargs):
    """Run a function, trapping any unhandled errors in Deferreds.

    Assumes that 'function' will have handled any errors in Deferreds by the
    time it is complete.  This is almost never true of any Twisted code, since
    you can never tell when someone has added an errback to a Deferred.

    If 'function' raises, then don't bother doing any unhandled error
    jiggery-pokery, since something horrible has probably happened anyway.

    :return: A tuple of '(result, error)', where 'result' is the value
        returned by 'function' and 'error' is a list of 'defer.DebugInfo'
        objects that have unhandled errors in Deferreds.
    """
    real_DebugInfo = defer.DebugInfo
    debug_infos = []
    def DebugInfo():
        info = real_DebugInfo()
        debug_infos.append(info)
        return info
    defer.DebugInfo = DebugInfo
    try:
        result = function(*args, **kwargs)
    finally:
        defer.DebugInfo = real_DebugInfo
    errors = []
    for info in debug_infos:
        if info.failResult is not None:
            errors.append(info)
            # Disable the destructor that logs to error. We are already
            # catching the error here.
            info.__del__ = lambda: None
    return result, errors


class TimeoutError(Exception):
    """Raised when run_in_reactor takes too long to run a function."""

    def __init__(self, function, timeout):
        Exception.__init__(self,
            "%r took longer than %s seconds" % (function, timeout))


class NoResultError(Exception):
    """Raised when the reactor has stopped but we don't have any result."""

    def __init__(self):
        Exception.__init__(self,
            "Tried to get test's result from Deferred when no result is "
            "available.  Probably means we received SIGINT or similar.")


class StaleJunkError(Exception):
    """Raised when there's junk in the spinner from a previous run."""

    def __init__(self, junk):
        Exception.__init__(self,
            "There was junk in the spinner from a previous run. "
            "Use clear_junk() to clear it out: %r" % (junk,))


class Spinner(object):
    """Spin the reactor until a function is done.

    This class emulates the behaviour of twisted.trial in that it grotesquely
    and horribly spins the Twisted reactor while a function is running, and
    then kills the reactor when that function is complete and all the
    callbacks in its chains are done.
    """

    _UNSET = object()

    # Signals that we save and restore for each spin.
    _PRESERVED_SIGNALS = [
        'SIGINT',
        'SIGTERM',
        'SIGCHLD',
        ]

    # There are many APIs within Twisted itself where a Deferred fires but
    # leaves cleanup work scheduled for the reactor to do.  Arguably, many of
    # these are bugs.  As such, we provide a facility to iterate the reactor
    # event loop a number of times after every call, in order to shake out
    # these buggy-but-commonplace events.  The default is 0, because that is
    # the ideal, and it actually works for many cases.
    _OBLIGATORY_REACTOR_ITERATIONS = 0

    def __init__(self, reactor, debug=False):
        """Construct a Spinner.

        :param reactor: A Twisted reactor.
        :param debug: Whether or not to enable Twisted's debugging.  Defaults
            to False.
        """
        self._reactor = reactor
        self._timeout_call = None
        self._success = self._UNSET
        self._failure = self._UNSET
        self._saved_signals = []
        self._junk = []
        self._debug = debug

    def _cancel_timeout(self):
        if self._timeout_call:
            self._timeout_call.cancel()

    def _get_result(self):
        if self._failure is not self._UNSET:
            self._failure.raiseException()
        if self._success is not self._UNSET:
            return self._success
        raise NoResultError()

    def _got_failure(self, result):
        self._cancel_timeout()
        self._failure = result

    def _got_success(self, result):
        self._cancel_timeout()
        self._success = result

    def _stop_reactor(self, ignored=None):
        """Stop the reactor!"""
        self._reactor.crash()

    def _timed_out(self, function, timeout):
        e = TimeoutError(function, timeout)
        self._failure = Failure(e)
        self._stop_reactor()

    def _clean(self):
        """Clean up any junk in the reactor.

        Will always iterate the reactor a number of times equal to
        ``Spinner._OBLIGATORY_REACTOR_ITERATIONS``.  This is to work around
        bugs in various Twisted APIs where a Deferred fires but still leaves
        work (e.g. cancelling a call, actually closing a connection) for the
        reactor to do.
        """
        for i in range(self._OBLIGATORY_REACTOR_ITERATIONS):
            self._reactor.iterate(0)
        junk = []
        for delayed_call in self._reactor.getDelayedCalls():
            delayed_call.cancel()
            junk.append(delayed_call)
        for selectable in self._reactor.removeAll():
            # Twisted sends a 'KILL' signal to selectables that provide
            # IProcessTransport.  Since only _dumbwin32proc processes do this,
            # we aren't going to bother.
            junk.append(selectable)
        if IReactorThreads.providedBy(self._reactor):
            if self._reactor.threadpool is not None:
                self._reactor._stopThreadPool()
        self._junk.extend(junk)
        return junk

    def clear_junk(self):
        """Clear out our recorded junk.

        :return: Whatever junk was there before.
        """
        junk = self._junk
        self._junk = []
        return junk

    def get_junk(self):
        """Return any junk that has been found on the reactor."""
        return self._junk

    def _save_signals(self):
        available_signals = [
            getattr(signal, name, None) for name in self._PRESERVED_SIGNALS]
        self._saved_signals = [
            (sig, signal.getsignal(sig)) for sig in available_signals if sig]

    def _restore_signals(self):
        for sig, hdlr in self._saved_signals:
            signal.signal(sig, hdlr)
        self._saved_signals = []

    @not_reentrant
    def run(self, timeout, function, *args, **kwargs):
        """Run 'function' in a reactor.

        If 'function' returns a Deferred, the reactor will keep spinning until
        the Deferred fires and its chain completes or until the timeout is
        reached -- whichever comes first.

        :raise TimeoutError: If 'timeout' is reached before the Deferred
            returned by 'function' has completed its callback chain.
        :raise NoResultError: If the reactor is somehow interrupted before
            the Deferred returned by 'function' has completed its callback
            chain.
        :raise StaleJunkError: If there's junk in the spinner from a previous
            run.
        :return: Whatever is at the end of the function's callback chain.  If
            it's an error, then raise that.
        """
        debug = MonkeyPatcher()
        if self._debug:
            debug.add_patch(defer.Deferred, 'debug', True)
            debug.add_patch(DelayedCall, 'debug', True)
        debug.patch()
        try:
            junk = self.get_junk()
            if junk:
                raise StaleJunkError(junk)
            self._save_signals()
            self._timeout_call = self._reactor.callLater(
                timeout, self._timed_out, function, timeout)
            # Calling 'stop' on the reactor will make it impossible to
            # re-start the reactor.  Since the default signal handlers for
            # TERM, BREAK and INT all call reactor.stop(), we'll patch it over
            # with crash.  XXX: It might be a better idea to either install
            # custom signal handlers or to override the methods that are
            # Twisted's signal handlers.
            stop, self._reactor.stop = self._reactor.stop, self._reactor.crash
            def run_function():
                d = defer.maybeDeferred(function, *args, **kwargs)
                d.addCallbacks(self._got_success, self._got_failure)
                d.addBoth(self._stop_reactor)
            try:
                self._reactor.callWhenRunning(run_function)
                self._reactor.run()
            finally:
                self._reactor.stop = stop
                self._restore_signals()
            try:
                return self._get_result()
            finally:
                self._clean()
        finally:
            debug.restore()
