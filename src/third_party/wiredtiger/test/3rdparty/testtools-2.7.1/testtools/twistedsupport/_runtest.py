# Copyright (c) 2010-2016 testtools developers. See LICENSE for details.

"""Individual test case execution for tests that return Deferreds.

Example::

    class TwistedTests(testtools.TestCase):

        run_tests_with = AsynchronousDeferredRunTest

        def test_something(self):
            # Wait for 5 seconds and then fire with 'Foo'.
            d = Deferred()
            reactor.callLater(5, lambda: d.callback('Foo'))
            d.addCallback(self.assertEqual, 'Foo')
            return d

When ``test_something`` is run, ``AsynchronousDeferredRunTest`` will run the
reactor until ``d`` fires, and wait for all of its callbacks to be processed.
"""

__all__ = [
    'AsynchronousDeferredRunTest',
    'AsynchronousDeferredRunTestForBrokenTwisted',
    'SynchronousDeferredRunTest',
    'assert_fails_with',
    ]

import io
import warnings
import sys

from fixtures import CompoundFixture, Fixture

from testtools.content import Content, text_content
from testtools.content_type import UTF8_TEXT
from testtools.runtest import RunTest, _raise_force_fail_error
from ._deferred import extract_result
from ._spinner import (
    NoResultError,
    Spinner,
    TimeoutError,
    trap_unhandled_errors,
    )

from twisted.internet import defer
try:
    from twisted.logger import globalLogPublisher
except ImportError:
    globalLogPublisher = None
from twisted.python import log
try:
    from twisted.trial.unittest import _LogObserver
except ImportError:
    from twisted.trial._synctest import _LogObserver


class _DeferredRunTest(RunTest):
    """Base for tests that return Deferreds."""

    def _got_user_failure(self, failure, tb_label='traceback'):
        """We got a failure from user code."""
        return self._got_user_exception(
            (failure.type, failure.value, failure.getTracebackObject()),
            tb_label=tb_label)


class SynchronousDeferredRunTest(_DeferredRunTest):
    """Runner for tests that return synchronous Deferreds.

    This runner doesn't touch the reactor at all. It assumes that tests return
    Deferreds that have already fired.
    """

    def _run_user(self, function, *args):
        d = defer.maybeDeferred(function, *args)
        d.addErrback(self._got_user_failure)
        result = extract_result(d)
        return result


def _get_global_publisher_and_observers():
    """Return ``(log_publisher, observers)``.

    Twisted 15.2.0 changed the logging framework. This method will always
    return a tuple of the global log publisher and all observers associated
    with that publisher.
    """
    if globalLogPublisher is not None:
        # Twisted >= 15.2.0, with the new twisted.logger framework.
        # log.theLogPublisher.observers will only contain legacy observers;
        # we need to look at globalLogPublisher._observers, which contains
        # both legacy and modern observers, and add and remove them via
        # globalLogPublisher.  However, we must still add and remove the
        # observers we want to run with via log.theLogPublisher, because
        # _LogObserver may consider old keys and require them to be mapped.
        publisher = globalLogPublisher
        return (publisher, list(publisher._observers))
    else:
        publisher = log.theLogPublisher
        return (publisher, list(publisher.observers))


class _NoTwistedLogObservers(Fixture):
    """Completely but temporarily remove all Twisted log observers."""

    def _setUp(self):
        publisher, real_observers = _get_global_publisher_and_observers()
        for observer in reversed(real_observers):
            publisher.removeObserver(observer)
            self.addCleanup(publisher.addObserver, observer)


class _TwistedLogObservers(Fixture):
    """Temporarily add Twisted log observers."""

    def __init__(self, observers):
        super().__init__()
        self._observers = observers
        self._log_publisher = log.theLogPublisher

    def _setUp(self):
        for observer in self._observers:
            self._log_publisher.addObserver(observer)
            self.addCleanup(self._log_publisher.removeObserver, observer)


class _ErrorObserver(Fixture):
    """Capture errors logged while fixture is active."""

    def __init__(self, error_observer):
        super().__init__()
        self._error_observer = error_observer

    def _setUp(self):
        self.useFixture(_TwistedLogObservers([self._error_observer.gotEvent]))

    def flush_logged_errors(self, *error_types):
        """Clear errors of the given types from the logs.

        If no errors provided, clear all errors.

        :return: An iterable of errors removed from the logs.
        """
        return self._error_observer.flushErrors(*error_types)


class CaptureTwistedLogs(Fixture):
    """Capture all the Twisted logs and add them as a detail.

    Much of the time, you won't need to use this directly, as
    :py:class:`AsynchronousDeferredRunTest` captures Twisted logs when the
    ``store_twisted_logs`` is set to ``True`` (which it is by default).

    However, if you want to do custom processing of Twisted's logs, then this
    class can be useful.

    For example::

        class TwistedTests(TestCase):
            run_tests_with(
                partial(AsynchronousDeferredRunTest, store_twisted_logs=False))

            def setUp(self):
                super(TwistedTests, self).setUp()
                twisted_logs = self.useFixture(CaptureTwistedLogs())
                # ... do something with twisted_logs ...
    """

    LOG_DETAIL_NAME = 'twisted-log'

    def _setUp(self):
        logs = io.StringIO()
        full_observer = log.FileLogObserver(logs)
        self.useFixture(_TwistedLogObservers([full_observer.emit]))
        self.addDetail(self.LOG_DETAIL_NAME, Content(
            UTF8_TEXT, lambda: [logs.getvalue().encode("utf-8")]))


def run_with_log_observers(observers, function, *args, **kwargs):
    """Run 'function' with the given Twisted log observers."""
    warnings.warn(
        'run_with_log_observers is deprecated since 1.8.2.',
        DeprecationWarning, stacklevel=2)
    with _NoTwistedLogObservers():
        with _TwistedLogObservers(observers):
            return function(*args, **kwargs)


# Observer of the Twisted log that we install during tests.
#
# This is a global so that users can call flush_logged_errors errors in their
# test cases.
_log_observer = _LogObserver()


def flush_logged_errors(*error_types):
    """Flush errors of the given types from the global Twisted log.

    Any errors logged during a test will be bubbled up to the test result,
    marking the test as erroring. Use this function to declare that logged
    errors were expected behavior.

    For example::

        try:
            1/0
        except ZeroDivisionError:
            log.err()
        # Prevent logged ZeroDivisionError from failing the test.
        flush_logged_errors(ZeroDivisionError)

    :param error_types: A variable argument list of exception types.
    """
    # XXX: jml: I would like to deprecate this in favour of
    # _ErrorObserver.flush_logged_errors so that I can avoid mutable global
    # state. However, I don't know how to make the correct instance of
    # _ErrorObserver.flush_logged_errors available to the end user. I also
    # don't yet have a clear deprecation/migration path.
    return _log_observer.flushErrors(*error_types)


class AsynchronousDeferredRunTest(_DeferredRunTest):
    """Runner for tests that return Deferreds that fire asynchronously.

    Use this runner when you have tests that return Deferreds that will
    only fire if the reactor is left to spin for a while.
    """

    def __init__(self, case, handlers=None, last_resort=None, reactor=None,
                 timeout=0.005, debug=False, suppress_twisted_logging=True,
                 store_twisted_logs=True):
        """Construct an ``AsynchronousDeferredRunTest``.

        Please be sure to always use keyword syntax, not positional, as the
        base class may add arguments in future - and for core code
        compatibility with that we have to insert them before the local
        parameters.

        :param TestCase case: The `TestCase` to run.
        :param handlers: A list of exception handlers (ExceptionType, handler)
            where 'handler' is a callable that takes a `TestCase`, a
            ``testtools.TestResult`` and the exception raised.
        :param last_resort: Handler to call before re-raising uncatchable
            exceptions (those for which there is no handler).
        :param reactor: The Twisted reactor to use.  If not given, we use the
            default reactor.
        :param float timeout: The maximum time allowed for running a test.  The
            default is 0.005s.
        :param debug: Whether or not to enable Twisted's debugging.  Use this
            to get information about unhandled Deferreds and left-over
            DelayedCalls.  Defaults to False.
        :param bool suppress_twisted_logging: If True, then suppress Twisted's
            default logging while the test is being run. Defaults to True.
        :param bool store_twisted_logs: If True, then store the Twisted logs
            that took place during the run as the 'twisted-log' detail.
            Defaults to True.
        """
        super().__init__(
            case, handlers, last_resort)
        if reactor is None:
            from twisted.internet import reactor
        self._reactor = reactor
        self._timeout = timeout
        self._debug = debug
        self._suppress_twisted_logging = suppress_twisted_logging
        self._store_twisted_logs = store_twisted_logs

    @classmethod
    def make_factory(cls, reactor=None, timeout=0.005, debug=False,
                     suppress_twisted_logging=True, store_twisted_logs=True):
        """Make a factory that conforms to the RunTest factory interface.

        Example::

            class SomeTests(TestCase):
                # Timeout tests after two minutes.
                run_tests_with = AsynchronousDeferredRunTest.make_factory(
                    timeout=120)
        """
        # This is horrible, but it means that the return value of the method
        # will be able to be assigned to a class variable *and* also be
        # invoked directly.
        class AsynchronousDeferredRunTestFactory:
            def __call__(self, case, handlers=None, last_resort=None):
                return cls(
                    case, handlers, last_resort, reactor, timeout, debug,
                    suppress_twisted_logging, store_twisted_logs,
                )
        return AsynchronousDeferredRunTestFactory()

    @defer.inlineCallbacks
    def _run_cleanups(self):
        """Run the cleanups on the test case.

        We expect that the cleanups on the test case can also return
        asynchronous Deferreds.  As such, we take the responsibility for
        running the cleanups, rather than letting TestCase do it.
        """
        last_exception = None
        while self.case._cleanups:
            f, args, kwargs = self.case._cleanups.pop()
            d = defer.maybeDeferred(f, *args, **kwargs)
            try:
                yield d
            except Exception:
                exc_info = sys.exc_info()
                self.case._report_traceback(exc_info)
                last_exception = exc_info[1]
        defer.returnValue(last_exception)

    def _make_spinner(self):
        """Make the `Spinner` to be used to run the tests."""
        return Spinner(self._reactor, debug=self._debug)

    def _run_deferred(self):
        """Run the test, assuming everything in it is Deferred-returning.

        This should return a Deferred that fires with True if the test was
        successful and False if the test was not successful.  It should *not*
        call addSuccess on the result, because there's reactor clean up that
        we needs to be done afterwards.
        """
        fails = []

        def fail_if_exception_caught(exception_caught):
            if self.exception_caught == exception_caught:
                fails.append(None)

        def clean_up(ignored=None):
            """Run the cleanups."""
            d = self._run_cleanups()

            def clean_up_done(result):
                if result is not None:
                    self._exceptions.append(result)
                    fails.append(None)
            return d.addCallback(clean_up_done)

        def set_up_done(exception_caught):
            """Set up is done, either clean up or run the test."""
            if self.exception_caught == exception_caught:
                fails.append(None)
                return clean_up()
            else:
                d = self._run_user(self.case._run_test_method, self.result)
                d.addCallback(fail_if_exception_caught)
                d.addBoth(tear_down)
                return d

        def tear_down(ignored):
            d = self._run_user(self.case._run_teardown, self.result)
            d.addCallback(fail_if_exception_caught)
            d.addBoth(clean_up)
            return d

        def force_failure(ignored):
            if getattr(self.case, 'force_failure', None):
                d = self._run_user(_raise_force_fail_error)
                d.addCallback(fails.append)
                return d

        d = self._run_user(self.case._run_setup, self.result)
        d.addCallback(set_up_done)
        d.addBoth(force_failure)
        d.addBoth(lambda ignored: len(fails) == 0)
        return d

    def _log_user_exception(self, e):
        """Raise 'e' and report it as a user exception."""
        try:
            raise e
        except e.__class__:
            self._got_user_exception(sys.exc_info())

    def _blocking_run_deferred(self, spinner):
        try:
            return trap_unhandled_errors(
                spinner.run, self._timeout, self._run_deferred)
        except NoResultError:
            # We didn't get a result at all!  This could be for any number of
            # reasons, but most likely someone hit Ctrl-C during the test.
            self._got_user_exception(sys.exc_info())
            self.result.stop()
            return False, []
        except TimeoutError:
            # The function took too long to run.
            self._log_user_exception(TimeoutError(self.case, self._timeout))
            return False, []

    def _get_log_fixture(self):
        """Return the log fixture we're configured to use."""
        fixtures = []
        # TODO: Expose these fixtures and deprecate both of these options in
        # favour of them.
        if self._suppress_twisted_logging:
            fixtures.append(_NoTwistedLogObservers())
        if self._store_twisted_logs:
            fixtures.append(CaptureTwistedLogs())
        return CompoundFixture(fixtures)

    def _run_core(self):
        # XXX: Blatting over the namespace of the test case isn't a nice thing
        # to do. Find a better way of communicating between runtest and test
        # case.
        self.case.reactor = self._reactor
        spinner = self._make_spinner()

        # We can't just install these as fixtures on self.case, because we
        # need the clean up to run even if the test times out.
        #
        # See https://bugs.launchpad.net/testtools/+bug/897196.
        with self._get_log_fixture() as capture_logs:
            for name, detail in capture_logs.getDetails().items():
                self.case.addDetail(name, detail)
            with _ErrorObserver(_log_observer) as error_fixture:
                successful, unhandled = self._blocking_run_deferred(
                    spinner)
            for logged_error in error_fixture.flush_logged_errors():
                successful = False
                self._got_user_failure(
                    logged_error, tb_label='logged-error')

        if unhandled:
            successful = False
            for debug_info in unhandled:
                f = debug_info.failResult
                info = debug_info._getDebugTracebacks()
                if info:
                    self.case.addDetail(
                        'unhandled-error-in-deferred-debug',
                        text_content(info))
                self._got_user_failure(f, 'unhandled-error-in-deferred')

        junk = spinner.clear_junk()
        if junk:
            successful = False
            self._log_user_exception(UncleanReactorError(junk))

        if successful:
            self.result.addSuccess(self.case, details=self.case.getDetails())

    def _run_user(self, function, *args):
        """Run a user-supplied function.

        This just makes sure that it returns a Deferred, regardless of how the
        user wrote it.
        """
        d = defer.maybeDeferred(function, *args)
        return d.addErrback(self._got_user_failure)


class AsynchronousDeferredRunTestForBrokenTwisted(AsynchronousDeferredRunTest):
    """Test runner that works around Twisted brokenness re reactor junk.

    There are many APIs within Twisted itself where a Deferred fires but
    leaves cleanup work scheduled for the reactor to do.  Arguably, many of
    these are bugs.  This runner iterates the reactor event loop a number of
    times after every test, in order to shake out these buggy-but-commonplace
    events.
    """

    def _make_spinner(self):
        spinner = super()._make_spinner()
        spinner._OBLIGATORY_REACTOR_ITERATIONS = 2
        return spinner


def assert_fails_with(d, *exc_types, **kwargs):
    """Assert that ``d`` will fail with one of ``exc_types``.

    The normal way to use this is to return the result of
    ``assert_fails_with`` from your unit test.

    Equivalent to Twisted's ``assertFailure``.

    :param Deferred d: A ``Deferred`` that is expected to fail.
    :param exc_types: The exception types that the Deferred is expected to
        fail with.
    :param type failureException: An optional keyword argument.  If provided,
        will raise that exception instead of
        ``testtools.TestCase.failureException``.
    :return: A ``Deferred`` that will fail with an ``AssertionError`` if ``d``
        does not fail with one of the exception types.
    """
    failureException = kwargs.pop('failureException', None)
    if failureException is None:
        # Avoid circular imports.
        from testtools import TestCase
        failureException = TestCase.failureException
    expected_names = ", ".join(exc_type.__name__ for exc_type in exc_types)

    def got_success(result):
        raise failureException(
            f"{expected_names} not raised ({result!r} returned)")

    def got_failure(failure):
        if failure.check(*exc_types):
            return failure.value
        raise failureException("{} raised instead of {}:\n {}".format(
            failure.type.__name__, expected_names, failure.getTraceback()))
    return d.addCallbacks(got_success, got_failure)


class UncleanReactorError(Exception):
    """Raised when the reactor has junk in it."""

    def __init__(self, junk):
        Exception.__init__(
            self,
            "The reactor still thinks it needs to do things. Close all "
            "connections, kill all processes and make sure all delayed "
            "calls have either fired or been cancelled:\n%s"
            % ''.join(map(self._get_junk_info, junk)))

    def _get_junk_info(self, junk):
        from twisted.internet.base import DelayedCall
        if isinstance(junk, DelayedCall):
            ret = str(junk)
        else:
            ret = repr(junk)
        return f'  {ret}\n'
