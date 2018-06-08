"""Enable running tests simultaneously by processing them from a multi-consumer queue."""

from __future__ import absolute_import

import sys

from .. import config
from .. import errors
from .. import logging
from ..testing.hooks import stepdown
from ..utils import queue as _queue


class Job(object):
    """Run tests from a queue."""

    def __init__(  # pylint: disable=too-many-arguments
            self, logger, fixture, hooks, report, archival, suite_options):
        """Initialize the job with the specified fixture and hooks."""

        self.logger = logger
        self.fixture = fixture
        self.hooks = hooks
        self.report = report
        self.archival = archival
        self.suite_options = suite_options

        # Don't check fixture.is_running() when using the ContinuousStepdown hook, which kills
        # and restarts the primary. Even if the fixture is still running as expected, there is a
        # race where fixture.is_running() could fail if called after the primary was killed but
        # before it was restarted.
        self._check_if_fixture_running = not any(
            isinstance(hook, stepdown.ContinuousStepdown) for hook in self.hooks)

    def __call__(self, queue, interrupt_flag, teardown_flag=None):
        """Continuously execute tests from 'queue' and records their details in 'report'.

        If 'teardown_flag' is not None, then 'self.fixture.teardown()'
        will be called before this method returns. If an error occurs
        while destroying the fixture, then the 'teardown_flag' will be
        set.
        """

        should_stop = False
        try:
            self._run(queue, interrupt_flag)
        except errors.StopExecution as err:
            # Stop running tests immediately.
            self.logger.error("Received a StopExecution exception: %s.", err)
            should_stop = True
        except:  # pylint: disable=bare-except
            # Unknown error, stop execution.
            self.logger.exception("Encountered an error during test execution.")
            should_stop = True

        # We give up on running more tests if the log output from an earlier run test ended up being
        # incomplete. Checking for should_stop=True isn't sufficient because it is possible for the
        # flush thread rather than another job thread to have called set_log_output_incomplete().
        if should_stop or logging.buildlogger.is_log_output_incomplete():
            # Set the interrupt flag so that other jobs do not start running more tests.
            interrupt_flag.set()
            # Drain the queue to unblock the main thread.
            Job._drain_queue(queue)

        if teardown_flag is not None:
            try:
                self.fixture.teardown(finished=True)
            except errors.ServerFailure as err:
                self.logger.warn("Teardown of %s was not successful: %s", self.fixture, err)
                teardown_flag.set()
            except:  # pylint: disable=bare-except
                self.logger.exception("Encountered an error while tearing down %s.", self.fixture)
                teardown_flag.set()

    def _run(self, queue, interrupt_flag):
        """Call the before/after suite hooks and continuously execute tests from 'queue'."""

        for hook in self.hooks:
            hook.before_suite(self.report)

        while not interrupt_flag.is_set():
            test = queue.get_nowait()
            try:
                if test is None:
                    # Sentinel value received, so exit.
                    break
                self._execute_test(test)
            finally:
                queue.task_done()

        for hook in self.hooks:
            hook.after_suite(self.report)

    def _execute_test(self, test):
        """Call the before/after test hooks and execute 'test'."""

        test.configure(self.fixture, config.NUM_CLIENTS_PER_FIXTURE)
        self._run_hooks_before_tests(test)

        test(self.report)
        try:
            # We are intentionally only checking the individual 'test' status and not calling
            # report.wasSuccessful() here. It is possible that a thread running in the background as
            # part of a hook has added a failed test case to 'self.report'. Checking the individual
            # 'test' status ensures self._run_hooks_after_tests() is called if it is a hook's test
            # case that has failed and not 'test' that has failed.
            if self.suite_options.fail_fast and self.report.find_test_info(test).status != "pass":
                self.logger.info("%s failed, so stopping..." % (test.short_description()))
                raise errors.StopExecution("%s failed" % (test.short_description()))

            if self._check_if_fixture_running and not self.fixture.is_running():
                self.logger.error(
                    "%s marked as a failure because the fixture crashed during the test.",
                    test.short_description())
                self.report.setFailure(test, return_code=2)
                # Always fail fast if the fixture fails.
                raise errors.StopExecution("%s not running after %s" % (self.fixture,
                                                                        test.short_description()))
        finally:
            success = self.report.find_test_info(test).status == "pass"
            if self.archival:
                self.archival.archive(self.logger, test, success)

        self._run_hooks_after_tests(test)

    def _run_hook(self, hook, hook_function, test):
        """Provide helper to run hook and archival."""
        try:
            success = False
            hook_function(test, self.report)
            success = True
        finally:
            if self.archival:
                self.archival.archive(self.logger, test, success, hook=hook)

    def _run_hooks_before_tests(self, test):
        """Run the before_test method on each of the hooks.

        Swallows any TestFailure exceptions if set to continue on
        failure, and reraises any other exceptions.
        """
        try:
            for hook in self.hooks:
                self._run_hook(hook, hook.before_test, test)

        except errors.StopExecution:
            raise

        except errors.ServerFailure:
            self.logger.exception("%s marked as a failure by a hook's before_test.",
                                  test.short_description())
            self._fail_test(test, sys.exc_info(), return_code=2)
            raise errors.StopExecution("A hook's before_test failed")

        except errors.TestFailure:
            self.logger.exception("%s marked as a failure by a hook's before_test.",
                                  test.short_description())
            self._fail_test(test, sys.exc_info(), return_code=1)
            if self.suite_options.fail_fast:
                raise errors.StopExecution("A hook's before_test failed")

        except:
            # Record the before_test() error in 'self.report'.
            self.report.startTest(test)
            self.report.addError(test, sys.exc_info())
            self.report.stopTest(test)
            raise

    def _run_hooks_after_tests(self, test):
        """Run the after_test method on each of the hooks.

        Swallows any TestFailure exceptions if set to continue on
        failure, and reraises any other exceptions.
        """
        try:
            for hook in self.hooks:
                self._run_hook(hook, hook.after_test, test)

        except errors.StopExecution:
            raise

        except errors.ServerFailure:
            self.logger.exception("%s marked as a failure by a hook's after_test.",
                                  test.short_description())
            self.report.setFailure(test, return_code=2)
            raise errors.StopExecution("A hook's after_test failed")

        except errors.TestFailure:
            self.logger.exception("%s marked as a failure by a hook's after_test.",
                                  test.short_description())
            self.report.setFailure(test, return_code=1)
            if self.suite_options.fail_fast:
                raise errors.StopExecution("A hook's after_test failed")

        except:
            self.report.setError(test)
            raise

    def _fail_test(self, test, exc_info, return_code=1):
        """Provide helper to record a test as a failure with the provided return code.

        This method should not be used if 'test' has already been
        started, instead use TestReport.setFailure().
        """

        self.report.startTest(test)
        test.return_code = return_code
        self.report.addFailure(test, exc_info)
        self.report.stopTest(test)

    @staticmethod
    def _drain_queue(queue):
        """Remove all elements from 'queue' without actually doing anything to them.

        Necessary to unblock the main thread that is waiting for 'queue' to be empty.
        """

        try:
            while not queue.empty():
                queue.get_nowait()
                queue.task_done()
        except _queue.Empty:
            # Multiple threads may be draining the queue simultaneously, so just ignore the
            # exception from the race between queue.empty() being false and failing to get an item.
            pass
