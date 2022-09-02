"""Driver of the test execution framework."""

import threading
import time
from typing import List

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import logging
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.core import network
from buildscripts.resmokelib.testing import fixtures
from buildscripts.resmokelib.testing import hook_test_archival as archival
from buildscripts.resmokelib.testing import hooks as _hooks
from buildscripts.resmokelib.testing import job as _job
from buildscripts.resmokelib.testing import report as _report
from buildscripts.resmokelib.testing import testcases
from buildscripts.resmokelib.testing.queue_element import queue_elem_factory, QueueElem
from buildscripts.resmokelib.utils import queue as _queue


class TestSuiteExecutor(object):
    """Execute a test suite.

    Responsible for setting up and tearing down the fixtures that the
    tests execute against.
    """

    _TIMEOUT = 24 * 60 * 60  # =1 day (a long time to have tests run)

    def __init__(self, exec_logger, suite, config=None, fixture=None, hooks=None,
                 archive_instance=None, archive=None):
        """Initialize the TestSuiteExecutor with the test suite to run."""
        self.logger = exec_logger

        if _config.SHELL_CONN_STRING is not None:
            # Specifying the shellConnString command line option should override the fixture
            # specified in the YAML configuration to be the external fixture.
            self.fixture_config = {
                "class": fixtures.EXTERNAL_FIXTURE_CLASS,
                "shell_conn_string": _config.SHELL_CONN_STRING
            }
        else:
            self.fixture_config = fixture

        self.hooks_config = utils.default_if_none(hooks, [])
        self.test_config = utils.default_if_none(config, {})

        self.archival = None
        if archive_instance:
            self.archival = archival.HookTestArchival(suite, self.hooks_config, archive_instance,
                                                      archive)

        self._suite = suite
        self.test_queue_logger = logging.loggers.new_testqueue_logger(suite.test_kind)

        # Must be done after getting buildlogger configuration.
        self._jobs = self._create_jobs(suite.get_num_jobs_to_start())

    def _create_jobs(self, num_jobs):
        """
        Start jobs.

        :return: List of jobs.
        """
        return [self._make_job(job_num) for job_num in range(num_jobs)]

    def run(self):
        """Execute the test suite.

        Any exceptions that occur during setting up or tearing down a
        fixture are propagated.
        """

        self.logger.info("Starting execution of %ss...", self._suite.test_kind)

        return_code = 0
        # The first run of the job will set up the fixture.
        setup_flag = threading.Event()
        # We reset the internal state of the PortAllocator so that ports used by the fixture during
        # a test suite run earlier can be reused during this current test suite.
        network.PortAllocator.reset()
        teardown_flag = None
        try:
            num_repeat_suites = self._suite.options.num_repeat_suites
            while num_repeat_suites > 0:
                test_queue = self._make_test_queue()

                partial_reports = [job.report for job in self._jobs]
                self._suite.record_test_start(partial_reports)

                # Have the Job threads destroy their fixture during the final repetition after they
                # finish running their last test. This avoids having a large number of processes
                # still running if an Evergreen task were to time out from a hang/deadlock being
                # triggered.
                teardown_flag = threading.Event() if num_repeat_suites == 1 else None
                # We use the 'hook_failure_flag' to distinguish hook failures from other failures,
                # so that we can return a separate return code when a hook has failed.
                hook_failure_flag = threading.Event()
                (report, interrupted) = self._run_tests(test_queue, setup_flag, teardown_flag,
                                                        hook_failure_flag)

                self._suite.record_test_end(report)

                if setup_flag and setup_flag.is_set():
                    self.logger.error("Setup of one of the job fixtures failed")
                    return_code = max(return_code, 2)
                    return
                # Remove the setup flag once the first suite ran.
                setup_flag = None

                if hook_failure_flag.is_set():
                    # The hook failure return code is highest so it will take precedence when
                    # reported.
                    return_code = max(return_code, 3)

                # If the user triggered a KeyboardInterrupt, then we should stop.
                if interrupted:
                    raise errors.UserInterrupt("Received interrupt from user")

                if teardown_flag and teardown_flag.is_set():
                    return_code = max(return_code, 2)

                sb = []  # String builder.
                self._suite.summarize_latest(sb)
                self.logger.info("Summary of latest execution: %s", "\n    ".join(sb))

                if not report.wasSuccessful():
                    return_code = max(return_code, 1)
                    if self._suite.options.fail_fast:
                        break

                test_report = report.as_dict()
                test_results_num = len(test_report["results"])
                # There should be at least as many tests results as expected number of tests.
                if test_results_num < test_queue.num_tests:
                    raise errors.ResmokeError(
                        "{} reported tests is less than {} expected tests".format(
                            test_results_num, test_queue.num_tests))

                # Clear the report so it can be reused for the next execution.
                for job in self._jobs:
                    job.report.reset()
                num_repeat_suites -= 1
        finally:
            if not teardown_flag:
                if not self._teardown_fixtures():
                    return_code = max(return_code, 2)

            self._suite.return_code = return_code

    def _run_tests(self, test_queue, setup_flag, teardown_flag, hook_failure_flag):
        """Start a thread for each Job instance and block until all of the tests are run.

        Returns a (combined report, user interrupted) pair, where the
        report contains the status and timing information of tests run
        by all of the threads.
        """

        threads = []
        interrupt_flag = threading.Event()
        user_interrupted = False
        try:
            # Run each Job instance in its own thread.
            for job in self._jobs:
                thr = threading.Thread(
                    target=job, args=(test_queue, interrupt_flag), kwargs=dict(
                        setup_flag=setup_flag, teardown_flag=teardown_flag,
                        hook_failure_flag=hook_failure_flag))
                # Do not wait for tests to finish executing if interrupted by the user.
                thr.daemon = True
                thr.start()
                threads.append(thr)
                # SERVER-24729 Need to stagger when jobs start to reduce I/O load if there
                # are many of them.  Both the 5 and the 10 are arbitrary.
                # Currently only enabled on Evergreen.
                if _config.STAGGER_JOBS and len(threads) >= 5:
                    time.sleep(10)

            joined = False
            while not joined:
                # Need to pass a timeout to join() so that KeyboardInterrupt exceptions
                # are propagated.
                joined = test_queue.join(TestSuiteExecutor._TIMEOUT)
        except (KeyboardInterrupt, SystemExit):
            interrupt_flag.set()
            user_interrupted = True

        wait_secs = 2.0
        self.logger.debug("Waiting for threads to complete")

        timer = threading.Timer(wait_secs, self._log_timeout_warning, args=[wait_secs])
        timer.daemon = True
        timer.start()
        try:
            for thr in threads:
                thr.join()
        finally:
            timer.cancel()

        self.logger.debug("Threads are completed!")

        reports = [job.report for job in self._jobs]
        combined_report = _report.TestReport.combine(*reports)

        # We cannot return 'interrupt_flag.is_set()' because the interrupt flag can be set by a Job
        # instance if a test fails and it decides to drain the queue. We only want to raise a
        # StopExecution exception in TestSuiteExecutor.run() if the user triggered the interrupt.
        return combined_report, user_interrupted

    def _teardown_fixtures(self):
        """Tear down all of the fixtures.

        Returns true if all fixtures were torn down successfully, and
        false otherwise.
        """
        success = True
        for job in self._jobs:
            if not job.manager.teardown_fixture(self.logger):
                self.logger.warning("Teardown of %s of job %s was not successful", job.fixture,
                                    job.job_num)
                success = False
        return success

    def _make_fixture(self, job_num):
        """Create a fixture for a job."""

        fixture_config = {}
        fixture_class = fixtures.NOOP_FIXTURE_CLASS
        if self.fixture_config is not None:
            fixture_config = self.fixture_config.copy()
            fixture_class = fixture_config.pop("class")

        fixture_logger = logging.loggers.new_fixture_logger(fixture_class, job_num)

        return fixtures.make_fixture(fixture_class, fixture_logger, job_num, **fixture_config)

    def _make_hooks(self, fixture, job_num):
        """Create the hooks for the job's fixture."""

        hooks = []

        for hook_config in self.hooks_config:
            hook_config = hook_config.copy()
            hook_class = hook_config.pop("class")

            hook_logger = logging.loggers.new_hook_logger(hook_class, job_num)
            hook = _hooks.make_hook(hook_class, hook_logger, fixture, **hook_config)
            hooks.append(hook)

        return hooks

    def _make_job(self, job_num):
        """
        Create a Job instance with its own fixture, hooks, and test report.

        :param job_num: instance number of job being created.
        :return: Job instance.
        """
        job_logger = logging.loggers.new_job_logger(self._suite.test_kind, job_num)

        fixture = self._make_fixture(job_num)
        hooks = self._make_hooks(fixture, job_num)

        report = _report.TestReport(job_logger, self._suite.options, job_num)

        return _job.Job(job_num, job_logger, fixture, hooks, report, self.archival,
                        self._suite.options, self.test_queue_logger)

    def _create_queue_elem_for_test_name(self, test_name):
        """
        Create the appropriate queue_elem to run the given test_name.

        :param test_name: Name of test to be queued.
        :return: queue_elem representing the test_name to be run.
        """
        test_case = testcases.make_test_case(self._suite.test_kind, self.test_queue_logger,
                                             test_name, **self.test_config)
        return queue_elem_factory(test_case, self.test_config, self._suite.options)

    def _make_test_queue(self):
        """
        Create a queue of test cases to run.

        Each test case will be added to the queue via a queue_element. For normal execution,
        we will add a queue_element for each run of the test (if we are repeating the tests twice,
        we will add 2 queue_elements of each test to the queue). If we are repeating execution for
        a specified time period, we will add each test to the queue, but as a QueueElemRepeatTime
        object, which will requeue itself if it has not run for the expected duration.
        Use a multi-consumer queue instead of a unittest.TestSuite so that the test cases can
        be dispatched to multiple threads.
        :return: Queue of testcases to run.
        """
        test_queue = TestQueue()
        test_cases = []

        # Make test cases to put in test queue
        for test_name in self._suite.make_test_case_names_list():
            queue_elem = self._create_queue_elem_for_test_name(test_name)
            test_cases.append(queue_elem)
        test_queue.add_test_cases(test_cases)

        return test_queue

    def _log_timeout_warning(self, seconds):
        """Log a message if any thread fails to terminate after `seconds`."""
        self.logger.warning(
            '*** Still waiting for processes to terminate after %s seconds. Try using ctrl-\\ '
            'to send a SIGQUIT on Linux or ctrl-c again on Windows ***', seconds)


class TestQueue(_queue.Queue):
    """A queue of test cases to run.

    Use a multi-consumer queue instead of a unittest.TestSuite so that the test cases can
    be dispatched to multiple threads.
    """

    def __init__(self):
        """Initialize test queue."""
        self.num_tests = 0
        self.max_test_queue_size = utils.default_if_none(_config.MAX_TEST_QUEUE_SIZE, -1)
        super().__init__()

    def add_test_cases(self, test_cases: List[QueueElem]) -> None:
        """Add test cases to the queue."""
        for test_case in test_cases:
            if self.max_test_queue_size < 0 or self.num_tests < self.max_test_queue_size:
                self.put(test_case)
                self.num_tests += 1
            else:
                break
