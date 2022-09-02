"""Enable running tests simultaneously by processing them from a multi-consumer queue."""

import sys
import time
from collections import namedtuple

from buildscripts.resmokelib import config
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing import testcases
from buildscripts.resmokelib.testing.fixtures.interface import create_fixture_table
from buildscripts.resmokelib.testing.testcases import fixture as _fixture
from buildscripts.resmokelib.utils import queue as _queue


class Job(object):
    """Run tests from a queue."""

    def __init__(self, job_num, logger, fixture, hooks, report, archival, suite_options,
                 test_queue_logger):
        """Initialize the job with the specified fixture and hooks."""

        self.logger = logger
        self.fixture = fixture
        self.hooks = hooks
        self.report = report
        self.archival = archival
        self.suite_options = suite_options
        self.manager = FixtureTestCaseManager(test_queue_logger, self.fixture, job_num, self.report)

        # Don't check fixture.is_running() when using hooks that kill and restart fixtures, such
        # as ContinuousStepdown or KillReplicator. Even if the fixture is still running as
        # expected, there is a race where fixture.is_running() could fail if called after the
        # primary was killed but before it was restarted.
        self._check_if_fixture_running = not any(
            hasattr(hook, "STOPS_FIXTURE") and hook.STOPS_FIXTURE for hook in self.hooks)

    @property
    def job_num(self):
        """Forward the job_num option from FixtureTestCaseManager."""
        return self.manager.job_num

    @staticmethod
    def _interrupt_all_jobs(queue, interrupt_flag):
        # Set the interrupt flag so that other jobs do not start running more tests.
        interrupt_flag.set()
        # Drain the queue to unblock the main thread.
        Job._drain_queue(queue)

    def __call__(self, queue, interrupt_flag, setup_flag=None, teardown_flag=None,
                 hook_failure_flag=None):
        """Continuously execute tests from 'queue' and records their details in 'report'.

        If 'setup_flag' is not None, then a test to set up the fixture will be run
        before running any other test. If an error occurs while setting up the fixture,
        then the 'setup_flag' will be set.
        If 'teardown_flag' is not None, then a test to tear down the fixture
        will be run before this method returns. If an error occurs
        while destroying the fixture, then the 'teardown_flag' will be set.
        """
        setup_succeeded = True
        if setup_flag is not None:
            try:
                setup_succeeded = self.manager.setup_fixture(self.logger)
            except errors.StopExecution as err:
                # Something went wrong when setting up the fixture. Perhaps we couldn't get a
                # test_id from logkeeper for where to put the log output. We don't attempt to run
                # any tests.
                self.logger.error(
                    "Received a StopExecution exception when setting up the fixture: %s.", err)
                setup_succeeded = False
            except:  # pylint: disable=bare-except
                # Something unexpected happened when setting up the fixture. We don't attempt to run
                # any tests.
                self.logger.exception("Encountered an error when setting up the fixture.")
                setup_succeeded = False

            if not setup_succeeded:
                setup_flag.set()
                self._interrupt_all_jobs(queue, interrupt_flag)

        if setup_succeeded:
            try:
                self._run(queue, interrupt_flag, teardown_flag, hook_failure_flag)
            except errors.StopExecution as err:
                # Stop running tests immediately.
                self.logger.error("Received a StopExecution exception: %s.", err)
                self._interrupt_all_jobs(queue, interrupt_flag)
            except:  # pylint: disable=bare-except
                # Unknown error, stop execution.
                self.logger.exception("Encountered an error during test execution.")
                self._interrupt_all_jobs(queue, interrupt_flag)

        if teardown_flag is not None:
            try:
                teardown_succeeded = self.manager.teardown_fixture(self.logger)
            except errors.StopExecution as err:
                # Something went wrong when tearing down the fixture. Perhaps we couldn't get a
                # test_id from logkeeper for where to put the log output. We indicate back to the
                # executor thread that teardown has failed. This likely means resmoke.py is exiting
                # without having terminated all of the child processes it spawned.
                self.logger.error(
                    "Received a StopExecution exception when tearing down the fixture: %s.", err)
                teardown_succeeded = False
            except:  # pylint: disable=bare-except
                # Something unexpected happened when tearing down the fixture. We indicate back to
                # the executor thread that teardown has failed. This may mean resmoke.py is exiting
                # without having terminated all of the child processes it spawned.
                self.logger.exception("Encountered an error when tearing down the fixture.")
                teardown_succeeded = False

            if not teardown_succeeded:
                teardown_flag.set()

    @staticmethod
    def _get_time():
        """Get current time to aid in the unit testing of the _run method."""
        return time.time()

    def _run(self, queue, interrupt_flag, teardown_flag=None, hook_failure_flag=None):
        """Call the before/after suite hooks and continuously execute tests from 'queue'."""

        self._run_hooks_before_suite(hook_failure_flag)

        while not queue.empty() and not interrupt_flag.is_set():
            queue_elem = queue.get_nowait()
            test_time_start = self._get_time()
            try:
                test = queue_elem.testcase
                self._execute_test(test, hook_failure_flag)
            finally:
                queue_elem.job_completed(self._get_time() - test_time_start)
                queue.task_done()

            self._requeue_test(queue, queue_elem, interrupt_flag)

        self._run_hooks_after_suite(teardown_flag, hook_failure_flag)

    def _log_requeue_test(self, queue_elem):
        """Log the requeue of a test."""

        if self.suite_options.time_repeat_tests_secs:
            progress = "{} of ({}/{}/{:2.2f} min/max/time)".format(
                queue_elem.repeat_num + 1, self.suite_options.num_repeat_tests_min,
                self.suite_options.num_repeat_tests_max, self.suite_options.time_repeat_tests_secs)
        else:
            progress = "{} of {}".format(queue_elem.repeat_num + 1,
                                         self.suite_options.num_repeat_tests)
        self.logger.info(("Requeueing test %s %s, cumulative time elapsed %0.2f"),
                         queue_elem.testcase.test_name, progress, queue_elem.repeat_time_elapsed)

    def _requeue_test(self, queue, queue_elem, interrupt_flag):
        """Requeue a test if it needs to be repeated."""

        if not queue_elem.should_requeue():
            return

        queue_elem.testcase = testcases.make_test_case(
            queue_elem.testcase.REGISTERED_NAME, queue_elem.testcase.logger,
            queue_elem.testcase.test_name, **queue_elem.test_config)

        if not interrupt_flag.is_set():
            self._log_requeue_test(queue_elem)
            queue.put(queue_elem)

    def _execute_test(self, test, hook_failure_flag):
        """Call the before/after test hooks and execute 'test'."""

        test.configure(self.fixture, config.NUM_CLIENTS_PER_FIXTURE)
        self._run_hooks_before_tests(test, hook_failure_flag)
        self.report.logging_prefix = create_fixture_table(self.fixture)

        test(self.report)
        try:
            if test.propagate_error is not None:
                raise test.propagate_error

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
                raise errors.StopExecution(
                    "%s not running after %s" % (self.fixture, test.short_description()))
        finally:
            success = self.report.find_test_info(test).status == "pass"

            # Stop background hooks first since they can interfere with fixture startup and teardown
            # done as part of archival.
            self._run_hooks_after_tests(test, hook_failure_flag, background=True)

            if self.archival:
                result = TestResult(test=test, hook=None, success=success)
                self.archival.archive(self.logger, result, self.manager)

            self._run_hooks_after_tests(test, hook_failure_flag, background=False)

    def _run_hook(self, hook, hook_function, test, hook_failure_flag):
        """Provide helper to run hook and archival."""
        try:
            success = False
            hook_function(test, self.report)
            success = True
        finally:
            if not success and hook_failure_flag is not None:
                hook_failure_flag.set()

            if self.archival:
                result = TestResult(test=test, hook=hook, success=success)
                self.archival.archive(self.logger, result, self.manager)

    def _run_hooks_before_suite(self, hook_failure_flag):
        """Run the before_suite method on each of the hooks."""
        hooks_failed = True
        try:
            for hook in self.hooks:
                hook.before_suite(self.report)
            hooks_failed = False
        finally:
            if hooks_failed and hook_failure_flag is not None:
                hook_failure_flag.set()

    def _run_hooks_after_suite(self, teardown_flag, hook_failure_flag):
        """Run the after_suite method on each of the hooks."""
        hooks_failed = True
        try:
            for hook in self.hooks:
                hook.after_suite(self.report, teardown_flag)
            hooks_failed = False
        finally:
            if hooks_failed and hook_failure_flag is not None:
                hook_failure_flag.set()

    def _run_hooks_before_tests(self, test, hook_failure_flag):
        """Run the before_test method on each of the hooks.

        Swallows any TestFailure exceptions if set to continue on
        failure, and reraises any other exceptions.
        """
        try:
            for hook in self.hooks:
                self._run_hook(hook, hook.before_test, test, hook_failure_flag)

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

    def _run_hooks_after_tests(self, test, hook_failure_flag, background=False):
        """Run the after_test method on each of the hooks.

        Swallows any TestFailure exceptions if set to continue on
        failure, and reraises any other exceptions.

        @param test: the test after which we run the hooks.
        @param background: whether to run background hooks.
        """
        try:
            for hook in self.hooks:
                if hook.IS_BACKGROUND == background:
                    self._run_hook(hook, hook.after_test, test, hook_failure_flag)

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


TestResult = namedtuple('TestResult', ['test', 'hook', 'success'])


class FixtureTestCaseManager:
    """Class that holds information needed to create new fixture setup/teardown test cases for a single job."""

    def __init__(self, test_queue_logger, fixture, job_num, report):
        """
        Initialize the test case manager.

        :param test_queue_logger: The logger associated with this job's test queue.
        :param fixture: The fixture associated with this job.
        :param job_num: This job's unique identifier.
        :param report: Report object collecting test results.
        """
        self.test_queue_logger = test_queue_logger
        self.fixture = fixture
        self.job_num = job_num
        self.report = report
        self.times_set_up = 0  # Setups and kills may run multiple times.

    def setup_fixture(self, logger):
        """
        Run a test that sets up the job's fixture and waits for it to be ready.

        Return True if the setup was successful, False otherwise.
        """
        test_case = _fixture.FixtureSetupTestCase(self.test_queue_logger, self.fixture,
                                                  "job{}".format(self.job_num), self.times_set_up)
        test_case(self.report)
        if self.report.find_test_info(test_case).status != "pass":
            logger.error("The setup of %s failed.", self.fixture)
            return False

        return True

    def teardown_fixture(self, logger, abort=False):
        """
        Run a test that tears down the job's fixture.

        Return True if the teardown was successful, False otherwise.
        """

        # Refresh the fixture table before teardown to capture changes due to
        # CleanEveryN and stepdown hooks.
        self.report.logging_prefix = create_fixture_table(self.fixture)

        if abort:
            test_case = _fixture.FixtureAbortTestCase(self.test_queue_logger, self.fixture,
                                                      "job{}".format(self.job_num),
                                                      self.times_set_up)
            self.times_set_up += 1
        else:
            test_case = _fixture.FixtureTeardownTestCase(self.test_queue_logger, self.fixture,
                                                         "job{}".format(self.job_num))

        test_case(self.report)
        if self.report.find_test_info(test_case).status != "pass":
            logger.error("The teardown of %s failed.", self.fixture)
            return False

        return True
