# Copyright (c) 2010-2016 testtools developers. See LICENSE for details.

"""Tests for the DeferredRunTest single test execution logic."""

import os
import signal

from testtools import (
    skipIf,
    TestCase,
    TestResult,
    )
from testtools.helpers import try_import
from testtools.matchers import (
    AfterPreprocessing,
    Contains,
    ContainsAll,
    ContainsDict,
    EndsWith,
    Equals,
    Is,
    KeysEqual,
    MatchesDict,
    MatchesListwise,
    Not,
    )
from testtools.runtest import RunTest
from testtools.testresult.doubles import ExtendedTestResult
from testtools.tests.helpers import (
    AsText,
    MatchesEvents,
)
from ._helpers import NeedsTwistedTestCase

DebugTwisted = try_import(
    'testtools.twistedsupport._deferreddebug.DebugTwisted')

assert_fails_with = try_import('testtools.twistedsupport.assert_fails_with')
AsynchronousDeferredRunTest = try_import(
    'testtools.twistedsupport.AsynchronousDeferredRunTest')
flush_logged_errors = try_import(
    'testtools.twistedsupport.flush_logged_errors')
SynchronousDeferredRunTest = try_import(
    'testtools.twistedsupport.SynchronousDeferredRunTest')

defer = try_import('twisted.internet.defer')
failure = try_import('twisted.python.failure')
log = try_import('twisted.python.log')
DelayedCall = try_import('twisted.internet.base.DelayedCall')
_get_global_publisher_and_observers = try_import(
    'testtools.twistedsupport._runtest._get_global_publisher_and_observers')


class X:
    """Tests that we run as part of our tests, nested to avoid discovery."""

    # XXX: After testing-cabal/testtools#165 lands, fix up all of these to be
    # scenario tests for RunTest.

    class Base(TestCase):
        def setUp(self):
            super(X.Base, self).setUp()
            self.calls = ['setUp']
            self.addCleanup(self.calls.append, 'clean-up')
        def test_something(self):
            self.calls.append('test')
        def tearDown(self):
            self.calls.append('tearDown')
            super(X.Base, self).tearDown()

    class BaseExceptionRaised(Base):
        expected_calls = ['setUp', 'tearDown', 'clean-up']
        expected_results = [('addError', SystemExit)]
        def test_something(self):
            raise SystemExit(0)

    class ErrorInSetup(Base):
        expected_calls = ['setUp', 'clean-up']
        expected_results = [('addError', RuntimeError)]
        def setUp(self):
            super(X.ErrorInSetup, self).setUp()
            raise RuntimeError("Error in setUp")

    class ErrorInTest(Base):
        expected_calls = ['setUp', 'tearDown', 'clean-up']
        expected_results = [('addError', RuntimeError)]
        def test_something(self):
            raise RuntimeError("Error in test")

    class FailureInTest(Base):
        expected_calls = ['setUp', 'tearDown', 'clean-up']
        expected_results = [('addFailure', AssertionError)]
        def test_something(self):
            self.fail("test failed")

    class ErrorInTearDown(Base):
        expected_calls = ['setUp', 'test', 'clean-up']
        expected_results = [('addError', RuntimeError)]
        def tearDown(self):
            raise RuntimeError("Error in tearDown")

    class ErrorInCleanup(Base):
        expected_calls = ['setUp', 'test', 'tearDown', 'clean-up']
        expected_results = [('addError', ZeroDivisionError)]
        def test_something(self):
            self.calls.append('test')
            self.addCleanup(lambda: 1/0)

    class ExpectThatFailure(Base):
        """Calling expectThat with a failing match fails the test."""
        expected_calls = ['setUp', 'test', 'tearDown', 'clean-up']
        expected_results = [('addFailure', AssertionError)]
        def test_something(self):
            self.calls.append('test')
            self.expectThat(object(), Is(object()))

    class TestIntegration(NeedsTwistedTestCase):

        def assertResultsMatch(self, test, result):
            events = list(result._events)
            self.assertEqual(('startTest', test), events.pop(0))
            for expected_result in test.expected_results:
                result = events.pop(0)
                if len(expected_result) == 1:
                    self.assertEqual((expected_result[0], test), result)
                else:
                    self.assertEqual((expected_result[0], test), result[:2])
                    error_type = expected_result[1]
                    self.assertIn(error_type.__name__, str(result[2]))
            self.assertEqual([('stopTest', test)], events)

        def test_runner(self):
            result = ExtendedTestResult()
            test = self.test_factory('test_something', runTest=self.runner)
            if self.test_factory is X.BaseExceptionRaised:
                self.assertRaises(SystemExit, test.run, result)
            else:
                test.run(result)
            self.assertEqual(test.calls, self.test_factory.expected_calls)
            self.assertResultsMatch(test, result)


def make_integration_tests():
    from unittest import TestSuite
    from testtools import clone_test_with_new_id
    runners = [
        ('RunTest', RunTest),
        ('SynchronousDeferredRunTest', SynchronousDeferredRunTest),
        ('AsynchronousDeferredRunTest', AsynchronousDeferredRunTest),
        ]

    tests = [
        X.BaseExceptionRaised,
        X.ErrorInSetup,
        X.ErrorInTest,
        X.ErrorInTearDown,
        X.FailureInTest,
        X.ErrorInCleanup,
        X.ExpectThatFailure,
    ]
    base_test = X.TestIntegration('test_runner')
    integration_tests = []
    for runner_name, runner in runners:
        for test in tests:
            new_test = clone_test_with_new_id(
                base_test, '{}({}, {})'.format(
                    base_test.id(),
                    runner_name,
                    test.__name__))
            new_test.test_factory = test
            new_test.runner = runner
            integration_tests.append(new_test)
    return TestSuite(integration_tests)


class TestSynchronousDeferredRunTest(NeedsTwistedTestCase):

    def make_result(self):
        return ExtendedTestResult()

    def make_runner(self, test):
        return SynchronousDeferredRunTest(test, test.exception_handlers)

    def test_success(self):
        class SomeCase(TestCase):
            def test_success(self):
                return defer.succeed(None)
        test = SomeCase('test_success')
        runner = self.make_runner(test)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            result._events, Equals([
                ('startTest', test),
                ('addSuccess', test),
                ('stopTest', test)]))

    def test_failure(self):
        class SomeCase(TestCase):
            def test_failure(self):
                return defer.maybeDeferred(self.fail, "Egads!")
        test = SomeCase('test_failure')
        runner = self.make_runner(test)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            [event[:2] for event in result._events], Equals([
                ('startTest', test),
                ('addFailure', test),
                ('stopTest', test)]))

    def test_setUp_followed_by_test(self):
        class SomeCase(TestCase):
            def setUp(self):
                super().setUp()
                return defer.succeed(None)
            def test_failure(self):
                return defer.maybeDeferred(self.fail, "Egads!")
        test = SomeCase('test_failure')
        runner = self.make_runner(test)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            [event[:2] for event in result._events], Equals([
                ('startTest', test),
                ('addFailure', test),
                ('stopTest', test)]))


class TestAsynchronousDeferredRunTest(NeedsTwistedTestCase):

    def make_reactor(self):
        from twisted.internet import reactor
        return reactor

    def make_result(self):
        return ExtendedTestResult()

    def make_runner(self, test, timeout=None, suppress_twisted_logging=True,
                    store_twisted_logs=True):
        if timeout is None:
            timeout = self.make_timeout()
        return AsynchronousDeferredRunTest(
            test, test.exception_handlers, timeout=timeout,
            suppress_twisted_logging=suppress_twisted_logging,
            store_twisted_logs=store_twisted_logs,
        )

    def make_timeout(self):
        return 0.005

    def test_setUp_returns_deferred_that_fires_later(self):
        # setUp can return a Deferred that might fire at any time.
        # AsynchronousDeferredRunTest will not go on to running the test until
        # the Deferred returned by setUp actually fires.
        call_log = []
        marker = object()
        d = defer.Deferred().addCallback(call_log.append)
        class SomeCase(TestCase):
            def setUp(self):
                super().setUp()
                call_log.append('setUp')
                return d
            def test_something(self):
                call_log.append('test')
        def fire_deferred():
            self.assertThat(call_log, Equals(['setUp']))
            d.callback(marker)
        test = SomeCase('test_something')
        timeout = self.make_timeout()
        runner = self.make_runner(test, timeout=timeout)
        result = self.make_result()
        reactor = self.make_reactor()
        reactor.callLater(timeout, fire_deferred)
        runner.run(result)
        self.assertThat(call_log, Equals(['setUp', marker, 'test']))

    def test_calls_setUp_test_tearDown_in_sequence(self):
        # setUp, the test method and tearDown can all return
        # Deferreds. AsynchronousDeferredRunTest will make sure that each of
        # these are run in turn, only going on to the next stage once the
        # Deferred from the previous stage has fired.
        call_log = []
        a = defer.Deferred()
        a.addCallback(lambda x: call_log.append('a'))
        b = defer.Deferred()
        b.addCallback(lambda x: call_log.append('b'))
        c = defer.Deferred()
        c.addCallback(lambda x: call_log.append('c'))
        class SomeCase(TestCase):
            def setUp(self):
                super().setUp()
                call_log.append('setUp')
                return a
            def test_success(self):
                call_log.append('test')
                return b
            def tearDown(self):
                super().tearDown()
                call_log.append('tearDown')
                return c
        test = SomeCase('test_success')
        timeout = self.make_timeout()
        runner = self.make_runner(test, timeout)
        result = self.make_result()
        reactor = self.make_reactor()
        def fire_a():
            self.assertThat(call_log, Equals(['setUp']))
            a.callback(None)
        def fire_b():
            self.assertThat(call_log, Equals(['setUp', 'a', 'test']))
            b.callback(None)
        def fire_c():
            self.assertThat(
                call_log, Equals(['setUp', 'a', 'test', 'b', 'tearDown']))
            c.callback(None)
        reactor.callLater(timeout * 0.25, fire_a)
        reactor.callLater(timeout * 0.5, fire_b)
        reactor.callLater(timeout * 0.75, fire_c)
        runner.run(result)
        self.assertThat(
            call_log, Equals(['setUp', 'a', 'test', 'b', 'tearDown', 'c']))

    def test_async_cleanups(self):
        # Cleanups added with addCleanup can return
        # Deferreds. AsynchronousDeferredRunTest will run each of them in
        # turn.
        class SomeCase(TestCase):
            def test_whatever(self):
                pass
        test = SomeCase('test_whatever')
        call_log = []
        a = defer.Deferred().addCallback(lambda x: call_log.append('a'))
        b = defer.Deferred().addCallback(lambda x: call_log.append('b'))
        c = defer.Deferred().addCallback(lambda x: call_log.append('c'))
        test.addCleanup(lambda: a)
        test.addCleanup(lambda: b)
        test.addCleanup(lambda: c)
        def fire_a():
            self.assertThat(call_log, Equals([]))
            a.callback(None)
        def fire_b():
            self.assertThat(call_log, Equals(['a']))
            b.callback(None)
        def fire_c():
            self.assertThat(call_log, Equals(['a', 'b']))
            c.callback(None)
        timeout = self.make_timeout()
        reactor = self.make_reactor()
        reactor.callLater(timeout * 0.25, fire_a)
        reactor.callLater(timeout * 0.5, fire_b)
        reactor.callLater(timeout * 0.75, fire_c)
        runner = self.make_runner(test, timeout)
        result = self.make_result()
        runner.run(result)
        self.assertThat(call_log, Equals(['a', 'b', 'c']))

    def test_clean_reactor(self):
        # If there's cruft left over in the reactor, the test fails.
        reactor = self.make_reactor()
        timeout = self.make_timeout()
        class SomeCase(TestCase):
            def test_cruft(self):
                reactor.callLater(timeout * 10.0, lambda: None)
        test = SomeCase('test_cruft')
        runner = self.make_runner(test, timeout)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            [event[:2] for event in result._events],
            Equals(
                [('startTest', test),
                 ('addError', test),
                 ('stopTest', test)]))
        error = result._events[1][2]
        self.assertThat(error, KeysEqual('traceback', 'twisted-log'))

    def test_exports_reactor(self):
        # The reactor is set as an attribute on the test case.
        reactor = self.make_reactor()
        timeout = self.make_timeout()
        class SomeCase(TestCase):
            def test_cruft(self):
                self.assertIs(reactor, self.reactor)
        test = SomeCase('test_cruft')
        runner = self.make_runner(test, timeout)
        result = TestResult()
        runner.run(result)
        self.assertEqual([], result.errors)
        self.assertEqual([], result.failures)

    def test_unhandled_error_from_deferred(self):
        # If there's a Deferred with an unhandled error, the test fails.  Each
        # unhandled error is reported with a separate traceback.

        # We're interested in the behavior when debugging is disabled. When
        # debugging is enabled, we get more stack traces.
        self.useFixture(DebugTwisted(False))
        class SomeCase(TestCase):
            def test_cruft(self):
                # Note we aren't returning the Deferred so that the error will
                # be unhandled.
                defer.maybeDeferred(lambda: 1/0)
                defer.maybeDeferred(lambda: 2/0)
        test = SomeCase('test_cruft')
        runner = self.make_runner(test)
        result = self.make_result()
        runner.run(result)
        error = result._events[1][2]
        result._events[1] = ('addError', test, None)
        self.assertThat(result._events, Equals(
            [('startTest', test),
             ('addError', test, None),
             ('stopTest', test)]))
        self.assertThat(
            error, KeysEqual(
                'twisted-log',
                'unhandled-error-in-deferred',
                'unhandled-error-in-deferred-1',
                ))

    def test_unhandled_error_from_deferred_combined_with_error(self):
        # If there's a Deferred with an unhandled error, the test fails.  Each
        # unhandled error is reported with a separate traceback, and the error
        # is still reported.

        # We're interested in the behavior when debugging is disabled. When
        # debugging is enabled, we get more stack traces.
        self.useFixture(DebugTwisted(False))
        class SomeCase(TestCase):
            def test_cruft(self):
                # Note we aren't returning the Deferred so that the error will
                # be unhandled.
                defer.maybeDeferred(lambda: 1/0)
                2 / 0
        test = SomeCase('test_cruft')
        runner = self.make_runner(test)
        result = self.make_result()
        runner.run(result)
        error = result._events[1][2]
        result._events[1] = ('addError', test, None)
        self.assertThat(result._events, Equals(
            [('startTest', test),
             ('addError', test, None),
             ('stopTest', test)]))
        self.assertThat(
            error, KeysEqual(
                'traceback',
                'twisted-log',
                'unhandled-error-in-deferred',
                ))

    @skipIf(os.name != "posix", "Sending SIGINT with os.kill is posix only")
    def test_keyboard_interrupt_stops_test_run(self):
        # If we get a SIGINT during a test run, the test stops and no more
        # tests run.
        SIGINT = getattr(signal, 'SIGINT', None)
        if not SIGINT:
            raise self.skipTest("SIGINT unavailable")
        class SomeCase(TestCase):
            def test_pause(self):
                return defer.Deferred()
        test = SomeCase('test_pause')
        reactor = self.make_reactor()
        timeout = self.make_timeout()
        runner = self.make_runner(test, timeout * 5)
        result = self.make_result()
        reactor.callLater(timeout, os.kill, os.getpid(), SIGINT)
        runner.run(result)
        self.assertThat(result.shouldStop, Equals(True))

    @skipIf(os.name != "posix", "Sending SIGINT with os.kill is posix only")
    def test_fast_keyboard_interrupt_stops_test_run(self):
        # If we get a SIGINT during a test run, the test stops and no more
        # tests run.
        SIGINT = getattr(signal, 'SIGINT', None)
        if not SIGINT:
            raise self.skipTest("SIGINT unavailable")
        class SomeCase(TestCase):
            def test_pause(self):
                return defer.Deferred()
        test = SomeCase('test_pause')
        reactor = self.make_reactor()
        timeout = self.make_timeout()
        runner = self.make_runner(test, timeout * 5)
        result = self.make_result()
        reactor.callWhenRunning(os.kill, os.getpid(), SIGINT)
        runner.run(result)
        self.assertThat(result.shouldStop, Equals(True))

    def test_timeout_causes_test_error(self):
        # If a test times out, it reports itself as having failed with a
        # TimeoutError.
        class SomeCase(TestCase):
            def test_pause(self):
                return defer.Deferred()
        test = SomeCase('test_pause')
        runner = self.make_runner(test)
        result = self.make_result()
        runner.run(result)
        error = result._events[1][2]
        self.assertThat(
            [event[:2] for event in result._events], Equals(
            [('startTest', test),
             ('addError', test),
             ('stopTest', test)]))
        self.assertIn('TimeoutError', str(error['traceback']))

    def test_convenient_construction(self):
        # As a convenience method, AsynchronousDeferredRunTest has a
        # classmethod that returns an AsynchronousDeferredRunTest
        # factory. This factory has the same API as the RunTest constructor.
        reactor = object()
        timeout = object()
        handler = object()
        factory = AsynchronousDeferredRunTest.make_factory(reactor, timeout)
        runner = factory(self, [handler])
        self.assertIs(reactor, runner._reactor)
        self.assertIs(timeout, runner._timeout)
        self.assertIs(self, runner.case)
        self.assertEqual([handler], runner.handlers)

    def test_use_convenient_factory(self):
        # Make sure that the factory can actually be used.
        factory = AsynchronousDeferredRunTest.make_factory()
        class SomeCase(TestCase):
            run_tests_with = factory
            def test_something(self):
                pass
        case = SomeCase('test_something')
        case.run()

    def test_convenient_construction_default_reactor(self):
        # As a convenience method, AsynchronousDeferredRunTest has a
        # classmethod that returns an AsynchronousDeferredRunTest
        # factory. This factory has the same API as the RunTest constructor.
        reactor = object()
        handler = object()
        factory = AsynchronousDeferredRunTest.make_factory(reactor=reactor)
        runner = factory(self, [handler])
        self.assertIs(reactor, runner._reactor)
        self.assertIs(self, runner.case)
        self.assertEqual([handler], runner.handlers)

    def test_convenient_construction_default_timeout(self):
        # As a convenience method, AsynchronousDeferredRunTest has a
        # classmethod that returns an AsynchronousDeferredRunTest
        # factory. This factory has the same API as the RunTest constructor.
        timeout = object()
        handler = object()
        factory = AsynchronousDeferredRunTest.make_factory(timeout=timeout)
        runner = factory(self, [handler])
        self.assertIs(timeout, runner._timeout)
        self.assertIs(self, runner.case)
        self.assertEqual([handler], runner.handlers)

    def test_convenient_construction_default_debugging(self):
        # As a convenience method, AsynchronousDeferredRunTest has a
        # classmethod that returns an AsynchronousDeferredRunTest
        # factory. This factory has the same API as the RunTest constructor.
        handler = object()
        factory = AsynchronousDeferredRunTest.make_factory(debug=True)
        runner = factory(self, [handler])
        self.assertIs(self, runner.case)
        self.assertEqual([handler], runner.handlers)
        self.assertEqual(True, runner._debug)

    def test_deferred_error(self):
        class SomeTest(TestCase):
            def test_something(self):
                return defer.maybeDeferred(lambda: 1/0)
        test = SomeTest('test_something')
        runner = self.make_runner(test)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            [event[:2] for event in result._events],
            Equals([
                ('startTest', test),
                ('addError', test),
                ('stopTest', test)]))
        error = result._events[1][2]
        self.assertThat(error, KeysEqual('traceback', 'twisted-log'))

    def test_only_addError_once(self):
        # Even if the reactor is unclean and the test raises an error and the
        # cleanups raise errors, we only called addError once per test.

        # We're interested in the behavior when debugging is disabled. When
        # debugging is enabled, we get more stack traces.
        self.useFixture(DebugTwisted(False))
        reactor = self.make_reactor()
        class WhenItRains(TestCase):
            def it_pours(self):
                # Add a dirty cleanup.
                self.addCleanup(lambda: 3 / 0)
                # Dirty the reactor.
                from twisted.internet.protocol import ServerFactory
                reactor.listenTCP(0, ServerFactory(), interface='127.0.0.1')
                # Unhandled error.
                defer.maybeDeferred(lambda: 2 / 0)
                # Actual error.
                raise RuntimeError("Excess precipitation")
        test = WhenItRains('it_pours')
        runner = self.make_runner(test)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            [event[:2] for event in result._events],
            Equals([
                ('startTest', test),
                ('addError', test),
                ('stopTest', test)]))
        error = result._events[1][2]
        self.assertThat(
            error, KeysEqual(
                'traceback',
                'traceback-1',
                'traceback-2',
                'twisted-log',
                'unhandled-error-in-deferred',
                ))

    def test_log_err_is_error(self):
        # An error logged during the test run is recorded as an error in the
        # tests.
        class LogAnError(TestCase):
            def test_something(self):
                try:
                    1/0
                except ZeroDivisionError:
                    f = failure.Failure()
                log.err(f)
        test = LogAnError('test_something')
        runner = self.make_runner(test, store_twisted_logs=False)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            result._events,
            MatchesEvents(
                ('startTest', test),
                ('addError', test, {
                    'logged-error': AsText(ContainsAll([
                        'Traceback (most recent call last):',
                        'ZeroDivisionError',
                        ])),
                    }),
                ('stopTest', test)))

    def test_log_err_flushed_is_success(self):
        # An error logged during the test run is recorded as an error in the
        # tests.
        class LogAnError(TestCase):
            def test_something(self):
                try:
                    1/0
                except ZeroDivisionError:
                    f = failure.Failure()
                log.err(f)
                flush_logged_errors(ZeroDivisionError)
        test = LogAnError('test_something')
        runner = self.make_runner(test, store_twisted_logs=False)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            result._events,
            MatchesEvents(
                ('startTest', test),
                ('addSuccess', test),
                ('stopTest', test)))

    def test_log_in_details(self):
        # If store_twisted_logs is True, we include the Twisted logs as
        # details.
        class LogAnError(TestCase):
            def test_something(self):
                log.msg("foo")
                1/0
        test = LogAnError('test_something')
        runner = self.make_runner(test, store_twisted_logs=True)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            result._events,
            MatchesEvents(
                ('startTest', test),
                ('addError', test, {
                    'traceback': Not(Is(None)),
                    'twisted-log': AsText(EndsWith(' foo\n')),
                    }),
                ('stopTest', test)))

    def test_do_not_log_to_twisted(self):
        # If suppress_twisted_logging is True, we don't log anything to the
        # default Twisted loggers.
        messages = []
        publisher, _ = _get_global_publisher_and_observers()
        publisher.addObserver(messages.append)
        self.addCleanup(publisher.removeObserver, messages.append)

        class LogSomething(TestCase):
            def test_something(self):
                log.msg("foo")

        test = LogSomething('test_something')
        runner = self.make_runner(test, suppress_twisted_logging=True)
        result = self.make_result()
        runner.run(result)
        self.assertThat(messages, Equals([]))

    def test_log_to_twisted(self):
        # If suppress_twisted_logging is False, we log to the default Twisted
        # loggers.
        messages = []
        publisher, _ = _get_global_publisher_and_observers()
        publisher.addObserver(messages.append)

        class LogSomething(TestCase):
            def test_something(self):
                log.msg("foo")

        test = LogSomething('test_something')
        runner = self.make_runner(test, suppress_twisted_logging=False)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            messages,
            MatchesListwise([ContainsDict({'message': Equals(('foo',))})]))

    def test_restore_observers(self):
        # We restore the original observers.
        publisher, observers = _get_global_publisher_and_observers()

        class LogSomething(TestCase):
            def test_something(self):
                pass

        test = LogSomething('test_something')
        runner = self.make_runner(test)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            _get_global_publisher_and_observers()[1], Equals(observers))

    def test_restore_observers_after_timeout(self):
        # We restore the original observers even if the test times out.
        publisher, observers = _get_global_publisher_and_observers()

        class LogSomething(TestCase):
            def test_something(self):
                return defer.Deferred()

        test = LogSomething('test_something')
        runner = self.make_runner(test, timeout=0.0001)
        result = self.make_result()
        runner.run(result)
        self.assertThat(
            _get_global_publisher_and_observers()[1], Equals(observers))

    def test_debugging_unchanged_during_test_by_default(self):
        debugging = [(defer.Deferred.debug, DelayedCall.debug)]
        class SomeCase(TestCase):
            def test_debugging_enabled(self):
                debugging.append((defer.Deferred.debug, DelayedCall.debug))
        test = SomeCase('test_debugging_enabled')
        runner = AsynchronousDeferredRunTest(
            test, handlers=test.exception_handlers,
            reactor=self.make_reactor(), timeout=self.make_timeout())
        runner.run(self.make_result())
        self.assertEqual(debugging[0], debugging[1])

    def test_debugging_enabled_during_test_with_debug_flag(self):
        self.patch(defer.Deferred, 'debug', False)
        self.patch(DelayedCall, 'debug', False)
        debugging = []
        class SomeCase(TestCase):
            def test_debugging_enabled(self):
                debugging.append((defer.Deferred.debug, DelayedCall.debug))
        test = SomeCase('test_debugging_enabled')
        runner = AsynchronousDeferredRunTest(
            test, handlers=test.exception_handlers,
            reactor=self.make_reactor(), timeout=self.make_timeout(),
            debug=True)
        runner.run(self.make_result())
        self.assertEqual([(True, True)], debugging)
        self.assertEqual(False, defer.Deferred.debug)
        self.assertEqual(False, defer.Deferred.debug)


class TestAssertFailsWith(NeedsTwistedTestCase):
    """Tests for `assert_fails_with`."""

    if SynchronousDeferredRunTest is not None:
        run_tests_with = SynchronousDeferredRunTest

    def test_assert_fails_with_success(self):
        # assert_fails_with fails the test if it's given a Deferred that
        # succeeds.
        marker = object()
        d = assert_fails_with(defer.succeed(marker), RuntimeError)
        def check_result(failure):
            failure.trap(self.failureException)
            self.assertThat(
                str(failure.value),
                Equals(f"RuntimeError not raised ({marker!r} returned)"))
        d.addCallbacks(
            lambda x: self.fail("Should not have succeeded"), check_result)
        return d

    def test_assert_fails_with_success_multiple_types(self):
        # assert_fails_with fails the test if it's given a Deferred that
        # succeeds.
        marker = object()
        d = assert_fails_with(
            defer.succeed(marker), RuntimeError, ZeroDivisionError)
        def check_result(failure):
            failure.trap(self.failureException)
            self.assertThat(
                str(failure.value),
                Equals("RuntimeError, ZeroDivisionError not raised "
                       "(%r returned)" % (marker,)))
        d.addCallbacks(
            lambda x: self.fail("Should not have succeeded"), check_result)
        return d

    def test_assert_fails_with_wrong_exception(self):
        # assert_fails_with fails the test if it's given a Deferred that
        # succeeds.
        d = assert_fails_with(
            defer.maybeDeferred(lambda: 1/0), RuntimeError, KeyboardInterrupt)
        def check_result(failure):
            failure.trap(self.failureException)
            lines = str(failure.value).splitlines()
            self.assertThat(
                lines[:2],
                Equals([
                    ("ZeroDivisionError raised instead of RuntimeError, "
                     "KeyboardInterrupt:"),
                    " Traceback (most recent call last):",
                    ]))
        d.addCallbacks(
            lambda x: self.fail("Should not have succeeded"), check_result)
        return d

    def test_assert_fails_with_expected_exception(self):
        # assert_fails_with calls back with the value of the failure if it's
        # one of the expected types of failures.
        try:
            1/0
        except ZeroDivisionError:
            f = failure.Failure()
        d = assert_fails_with(defer.fail(f), ZeroDivisionError)
        return d.addCallback(self.assertThat, Equals(f.value))

    def test_custom_failure_exception(self):
        # If assert_fails_with is passed a 'failureException' keyword
        # argument, then it will raise that instead of `AssertionError`.
        class CustomException(Exception):
            pass
        marker = object()
        d = assert_fails_with(
            defer.succeed(marker), RuntimeError,
            failureException=CustomException)
        def check_result(failure):
            failure.trap(CustomException)
            self.assertThat(
                str(failure.value),
                Equals(f"RuntimeError not raised ({marker!r} returned)"))
        return d.addCallbacks(
            lambda x: self.fail("Should not have succeeded"), check_result)


class TestRunWithLogObservers(NeedsTwistedTestCase):

    def test_restores_observers(self):
        from testtools.twistedsupport._runtest import run_with_log_observers
        from twisted.python import log
        # Make sure there's at least one observer.  This reproduces bug
        # #926189.
        log.addObserver(lambda *args: None)
        observers = list(log.theLogPublisher.observers)
        run_with_log_observers([], lambda: None)
        self.assertEqual(observers, log.theLogPublisher.observers)


class TestNoTwistedLogObservers(NeedsTwistedTestCase):
    """Tests for _NoTwistedLogObservers."""

    def _get_logged_messages(self, function, *args, **kwargs):
        """Run ``function`` and return ``(ret, logged_messages)``."""
        messages = []
        publisher, _ = _get_global_publisher_and_observers()
        publisher.addObserver(messages.append)
        try:
            ret = function(*args, **kwargs)
        finally:
            publisher.removeObserver(messages.append)
        return (ret, messages)

    def test_default(self):
        # The default behaviour is for messages logged to Twisted to actually
        # go to the Twisted logs.
        class SomeTest(TestCase):
            def test_something(self):
                log.msg('foo')

        _, messages = self._get_logged_messages(SomeTest('test_something').run)
        self.assertThat(
            messages,
            MatchesListwise([ContainsDict({'message': Equals(('foo',))})]))

    def test_nothing_logged(self):
        # Using _NoTwistedLogObservers means that nothing is logged to
        # Twisted.
        from testtools.twistedsupport._runtest import _NoTwistedLogObservers

        class SomeTest(TestCase):
            def test_something(self):
                self.useFixture(_NoTwistedLogObservers())
                log.msg('foo')

        _, messages = self._get_logged_messages(SomeTest('test_something').run)
        self.assertThat(messages, Equals([]))

    def test_logging_restored(self):
        # _NoTwistedLogObservers restores the original log observers.
        from testtools.twistedsupport._runtest import _NoTwistedLogObservers

        class SomeTest(TestCase):
            def test_something(self):
                self.useFixture(_NoTwistedLogObservers())
                log.msg('foo')

        def run_then_log():
            SomeTest('test_something').run()
            log.msg('bar')

        _, messages = self._get_logged_messages(run_then_log)
        self.assertThat(
            messages,
            MatchesListwise([ContainsDict({'message': Equals(('bar',))})]))


class TestTwistedLogObservers(NeedsTwistedTestCase):
    """Tests for _TwistedLogObservers."""

    def test_logged_messages_go_to_observer(self):
        # Using _TwistedLogObservers means messages logged to Twisted go to
        # that observer while the fixture is active.
        from testtools.twistedsupport._runtest import _TwistedLogObservers

        messages = []

        class SomeTest(TestCase):
            def test_something(self):
                self.useFixture(_TwistedLogObservers([messages.append]))
                log.msg('foo')

        SomeTest('test_something').run()
        log.msg('bar')
        self.assertThat(
            messages,
            MatchesListwise([ContainsDict({'message': Equals(('foo',))})]))


class TestErrorObserver(NeedsTwistedTestCase):
    """Tests for _ErrorObserver."""

    def test_captures_errors(self):
        # _ErrorObserver stores all errors logged while it is active.
        from testtools.twistedsupport._runtest import (
            _ErrorObserver, _LogObserver, _NoTwistedLogObservers)

        log_observer = _LogObserver()
        error_observer = _ErrorObserver(log_observer)
        exception = ValueError('bar')

        class SomeTest(TestCase):
            def test_something(self):
                # Temporarily suppress default log observers to avoid spewing
                # to stderr.
                self.useFixture(_NoTwistedLogObservers())
                self.useFixture(error_observer)
                log.msg('foo')
                log.err(exception)

        SomeTest('test_something').run()
        self.assertThat(
            error_observer.flush_logged_errors(),
            MatchesListwise([
                AfterPreprocessing(lambda x: x.value, Equals(exception))]))


class TestCaptureTwistedLogs(NeedsTwistedTestCase):
    """Tests for CaptureTwistedLogs."""

    def test_captures_logs(self):
        # CaptureTwistedLogs stores all Twisted log messages as a detail.
        from testtools.twistedsupport import CaptureTwistedLogs

        class SomeTest(TestCase):
            def test_something(self):
                self.useFixture(CaptureTwistedLogs())
                log.msg('foo')

        test = SomeTest('test_something')
        test.run()
        self.assertThat(
            test.getDetails(),
            MatchesDict({
                'twisted-log': AsText(Contains('foo')),
            }))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)


def load_tests(loader, tests, pattern):
    tests.addTest(make_integration_tests())
    return tests
