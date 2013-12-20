# Copyright (c) 2009-2011 testtools developers. See LICENSE for details.

"""Test suites and related things."""

__metaclass__ = type
__all__ = [
  'ConcurrentTestSuite',
  'ConcurrentStreamTestSuite',
  'filter_by_ids',
  'iterate_tests',
  'sorted_tests',
  ]

import sys
import threading
import unittest

from extras import safe_hasattr, try_imports

Queue = try_imports(['Queue.Queue', 'queue.Queue'])

import testtools


def iterate_tests(test_suite_or_case):
    """Iterate through all of the test cases in 'test_suite_or_case'."""
    try:
        suite = iter(test_suite_or_case)
    except TypeError:
        yield test_suite_or_case
    else:
        for test in suite:
            for subtest in iterate_tests(test):
                yield subtest


class ConcurrentTestSuite(unittest.TestSuite):
    """A TestSuite whose run() calls out to a concurrency strategy."""

    def __init__(self, suite, make_tests, wrap_result=None):
        """Create a ConcurrentTestSuite to execute suite.

        :param suite: A suite to run concurrently.
        :param make_tests: A helper function to split the tests in the
            ConcurrentTestSuite into some number of concurrently executing
            sub-suites. make_tests must take a suite, and return an iterable
            of TestCase-like object, each of which must have a run(result)
            method.
        :param wrap_result: An optional function that takes a thread-safe
            result and a thread number and must return a ``TestResult``
            object. If not provided, then ``ConcurrentTestSuite`` will just
            use a ``ThreadsafeForwardingResult`` wrapped around the result
            passed to ``run()``.
        """
        super(ConcurrentTestSuite, self).__init__([suite])
        self.make_tests = make_tests
        if wrap_result:
            self._wrap_result = wrap_result

    def _wrap_result(self, thread_safe_result, thread_number):
        """Wrap a thread-safe result before sending it test results.

        You can either override this in a subclass or pass your own
        ``wrap_result`` in to the constructor.  The latter is preferred.
        """
        return thread_safe_result

    def run(self, result):
        """Run the tests concurrently.

        This calls out to the provided make_tests helper, and then serialises
        the results so that result only sees activity from one TestCase at
        a time.

        ConcurrentTestSuite provides no special mechanism to stop the tests
        returned by make_tests, it is up to the make_tests to honour the
        shouldStop attribute on the result object they are run with, which will
        be set if an exception is raised in the thread which
        ConcurrentTestSuite.run is called in.
        """
        tests = self.make_tests(self)
        try:
            threads = {}
            queue = Queue()
            semaphore = threading.Semaphore(1)
            for i, test in enumerate(tests):
                process_result = self._wrap_result(
                    testtools.ThreadsafeForwardingResult(result, semaphore), i)
                reader_thread = threading.Thread(
                    target=self._run_test, args=(test, process_result, queue))
                threads[test] = reader_thread, process_result
                reader_thread.start()
            while threads:
                finished_test = queue.get()
                threads[finished_test][0].join()
                del threads[finished_test]
        except:
            for thread, process_result in threads.values():
                process_result.stop()
            raise

    def _run_test(self, test, process_result, queue):
        try:
            try:
                test.run(process_result)
            except Exception as e:
                # The run logic itself failed.
                case = testtools.ErrorHolder(
                    "broken-runner",
                    error=sys.exc_info())
                case.run(process_result)
        finally:
            queue.put(test)


class ConcurrentStreamTestSuite(object):
    """A TestSuite whose run() parallelises."""

    def __init__(self, make_tests):
        """Create a ConcurrentTestSuite to execute tests returned by make_tests.

        :param make_tests: A helper function that should return some number
            of concurrently executable test suite / test case objects.
            make_tests must take no parameters and return an iterable of
            tuples. Each tuple must be of the form (case, route_code), where
            case is a TestCase-like object with a run(result) method, and
            route_code is either None or a unicode string.
        """
        super(ConcurrentStreamTestSuite, self).__init__()
        self.make_tests = make_tests

    def run(self, result):
        """Run the tests concurrently.

        This calls out to the provided make_tests helper to determine the
        concurrency to use and to assign routing codes to each worker.

        ConcurrentTestSuite provides no special mechanism to stop the tests
        returned by make_tests, it is up to the made tests to honour the
        shouldStop attribute on the result object they are run with, which will
        be set if the test run is to be aborted.

        The tests are run with an ExtendedToStreamDecorator wrapped around a
        StreamToQueue instance. ConcurrentStreamTestSuite dequeues events from
        the queue and forwards them to result. Tests can therefore be either
        original unittest tests (or compatible tests), or new tests that emit
        StreamResult events directly.

        :param result: A StreamResult instance. The caller is responsible for
            calling startTestRun on this instance prior to invoking suite.run,
            and stopTestRun subsequent to the run method returning.
        """
        tests = self.make_tests()
        try:
            threads = {}
            queue = Queue()
            for test, route_code in tests:
                to_queue = testtools.StreamToQueue(queue, route_code)
                process_result = testtools.ExtendedToStreamDecorator(
                    testtools.TimestampingStreamResult(to_queue))
                runner_thread = threading.Thread(
                    target=self._run_test,
                    args=(test, process_result, route_code))
                threads[to_queue] = runner_thread, process_result
                runner_thread.start()
            while threads:
                event_dict = queue.get()
                event = event_dict.pop('event')
                if event == 'status':
                    result.status(**event_dict)
                elif event == 'stopTestRun':
                    thread = threads.pop(event_dict['result'])[0]
                    thread.join()
                elif event == 'startTestRun':
                    pass
                else:
                    raise ValueError('unknown event type %r' % (event,))
        except:
            for thread, process_result in threads.values():
                # Signal to each TestControl in the ExtendedToStreamDecorator
                # that the thread should stop running tests and cleanup
                process_result.stop()
            raise

    def _run_test(self, test, process_result, route_code):
        process_result.startTestRun()
        try:
            try:
                test.run(process_result)
            except Exception as e:
                # The run logic itself failed.
                case = testtools.ErrorHolder(
                    "broken-runner-'%s'" % (route_code,),
                    error=sys.exc_info())
                case.run(process_result)
        finally:
            process_result.stopTestRun()


class FixtureSuite(unittest.TestSuite):

    def __init__(self, fixture, tests):
        super(FixtureSuite, self).__init__(tests)
        self._fixture = fixture

    def run(self, result):
        self._fixture.setUp()
        try:
            super(FixtureSuite, self).run(result)
        finally:
            self._fixture.cleanUp()

    def sort_tests(self):
        self._tests = sorted_tests(self, True)


def _flatten_tests(suite_or_case, unpack_outer=False):
    try:
        tests = iter(suite_or_case)
    except TypeError:
        # Not iterable, assume it's a test case.
        return [(suite_or_case.id(), suite_or_case)]
    if (type(suite_or_case) in (unittest.TestSuite,) or
        unpack_outer):
        # Plain old test suite (or any others we may add).
        result = []
        for test in tests:
            # Recurse to flatten.
            result.extend(_flatten_tests(test))
        return result
    else:
        # Find any old actual test and grab its id.
        suite_id = None
        tests = iterate_tests(suite_or_case)
        for test in tests:
            suite_id = test.id()
            break
        # If it has a sort_tests method, call that.
        if safe_hasattr(suite_or_case, 'sort_tests'):
            suite_or_case.sort_tests()
        return [(suite_id, suite_or_case)]


def filter_by_ids(suite_or_case, test_ids):
    """Remove tests from suite_or_case where their id is not in test_ids.
    
    :param suite_or_case: A test suite or test case.
    :param test_ids: Something that supports the __contains__ protocol.
    :return: suite_or_case, unless suite_or_case was a case that itself
        fails the predicate when it will return a new unittest.TestSuite with
        no contents.

    This helper exists to provide backwards compatability with older versions
    of Python (currently all versions :)) that don't have a native
    filter_by_ids() method on Test(Case|Suite).

    For subclasses of TestSuite, filtering is done by:
        - attempting to call suite.filter_by_ids(test_ids)
        - if there is no method, iterating the suite and identifying tests to
          remove, then removing them from _tests, manually recursing into
          each entry.

    For objects with an id() method - TestCases, filtering is done by:
        - attempting to return case.filter_by_ids(test_ids)
        - if there is no such method, checking for case.id() in test_ids
          and returning case if it is, or TestSuite() if it is not.

    For anything else, it is not filtered - it is returned as-is.

    To provide compatability with this routine for a custom TestSuite, just
    define a filter_by_ids() method that will return a TestSuite equivalent to
    the original minus any tests not in test_ids.
    Similarly to provide compatability for a custom TestCase that does
    something unusual define filter_by_ids to return a new TestCase object
    that will only run test_ids that are in the provided container. If none
    would run, return an empty TestSuite().

    The contract for this function does not require mutation - each filtered
    object can choose to return a new object with the filtered tests. However
    because existing custom TestSuite classes in the wild do not have this
    method, we need a way to copy their state correctly which is tricky:
    thus the backwards-compatible code paths attempt to mutate in place rather
    than guessing how to reconstruct a new suite.
    """
    # Compatible objects
    if safe_hasattr(suite_or_case, 'filter_by_ids'):
        return suite_or_case.filter_by_ids(test_ids)
    # TestCase objects.
    if safe_hasattr(suite_or_case, 'id'):
        if suite_or_case.id() in test_ids:
            return suite_or_case
        else:
            return unittest.TestSuite()
    # Standard TestSuites or derived classes [assumed to be mutable].
    if isinstance(suite_or_case, unittest.TestSuite):
        filtered = []
        for item in suite_or_case:
            filtered.append(filter_by_ids(item, test_ids))
        suite_or_case._tests[:] = filtered
    # Everything else:
    return suite_or_case


def sorted_tests(suite_or_case, unpack_outer=False):
    """Sort suite_or_case while preserving non-vanilla TestSuites."""
    # Duplicate test id can induce TypeError in Python 3.3.
    # Detect the duplicate test id, raise exception when found.
    seen = set()
    for test_case in iterate_tests(suite_or_case):
        test_id = test_case.id()
        if test_id not in seen:
            seen.add(test_id)
        else:
            raise ValueError('Duplicate test id detected: %s' % (test_id,))
    tests = _flatten_tests(suite_or_case, unpack_outer=unpack_outer)
    tests.sort()
    return unittest.TestSuite([test for (sort_key, test) in tests])
