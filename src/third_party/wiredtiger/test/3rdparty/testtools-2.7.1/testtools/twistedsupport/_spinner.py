# Copyright (c) testtools developers. See LICENSE for details.

"""Evil reactor-spinning logic for running Twisted tests.

This code is highly experimental, liable to change and not to be trusted.  If
you couldn't write this yourself, you should not be using it.
"""

__all__ = [
    'NoResultError',
    'not_reentrant',
    'ReentryError',
    'Spinner',
    'StaleJunkError',
    'TimeoutError',
    'trap_unhandled_errors',
    ]

from fixtures import Fixture
import signal
from typing import Union

from ._deferreddebug import DebugTwisted

from twisted.internet import defer
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

    # Apparently the fact that Twisted now decorates DebugInfo with @_oldStyle
    # screws up things a bit for us here: monkey patching the __del__ method on
    # an instance doesn't work with Python 3 and viceversa overriding __del__
    # via inheritance doesn't work with Python 2. So we handle the two cases
    # differently. TODO: perhaps there's a way to have a single code path?
    class DebugInfo(real_DebugInfo):  # type: ignore

        _runRealDel = True

        def __init__(self):
            real_DebugInfo.__init__(self)
            debug_infos.append(self)

        def __del__(self):
            if self._runRealDel:
                real_DebugInfo.__del__(self)

    defer.DebugInfo = DebugInfo  # type: ignore
    try:
        result = function(*args, **kwargs)
    finally:
        defer.DebugInfo = real_DebugInfo  # type: ignore
    errors = []
    for info in debug_infos:
        if info.failResult is not None:
            errors.append(info)
            # Disable the destructor that logs to error. We are already
            # catching the error here.
            info._runRealDel = False
    return result, errors


class TimeoutError(Exception):
    """Raised when run_in_reactor takes too long to run a function."""

    def __init__(self, function, timeout):
        Exception.__init__(self,
            f"{function!r} took longer than {timeout} seconds")


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


class Spinner:
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

    _failure: Union[Failure, object]

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
        self._spinning = False

    def _cancel_timeout(self):
        if self._timeout_call:
            self._timeout_call.cancel()

    def _get_result(self):
        if self._failure is not self._UNSET:
            self._failure.raiseException()  # type: ignore
        if self._success is not self._UNSET:
            return self._success
        raise NoResultError()

    def _got_failure(self, result):
        self._cancel_timeout()
        self._failure = result

    def _got_success(self, result):
        self._cancel_timeout()
        self._success = result

    def _fake_stop(self):
        """Use to replace ``reactor.stop`` while running a test.

        Calling ``reactor.stop`` makes it impossible re-start the reactor.
        Since the default signal handlers for TERM, BREAK and INT all call
        ``reactor.stop()``, we patch it over with ``reactor.crash()``

        Spinner never calls this method.
        """
        self._reactor.crash()

    def _stop_reactor(self, ignored=None):
        """Stop the reactor!"""
        # XXX: Would like to emit a warning when called when *not* spinning.
        if self._spinning:
            self._reactor.crash()
            self._spinning = False

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
        if self._debug:
            debug_settings = DebugTwisted(True)
        else:
            debug_settings = Fixture()

        with debug_settings:
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
            real_stop, self._reactor.stop = self._reactor.stop, self._fake_stop

            def run_function():
                d = defer.maybeDeferred(function, *args, **kwargs)
                d.addCallbacks(self._got_success, self._got_failure)
                d.addBoth(self._stop_reactor)
            try:
                self._reactor.callWhenRunning(run_function)
                self._spinning = True
                self._reactor.run()
            finally:
                self._reactor.stop = real_stop
                self._restore_signals()
            try:
                return self._get_result()
            finally:
                self._clean()
