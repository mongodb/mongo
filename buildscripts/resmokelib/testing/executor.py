"""
Driver of the test execution framework.
"""

from __future__ import absolute_import

import threading
import time

from . import fixtures
from . import hooks as _hooks
from . import job as _job
from . import report as _report
from . import testcases
from .. import config as _config
from .. import errors
from .. import utils
from ..core import network
from ..utils import queue as _queue


class TestSuiteExecutor(object):
    """
    Executes a test suite.

    Responsible for setting up and tearing down the fixtures that the
    tests execute against.
    """

    _TIMEOUT = 24 * 60 * 60  # =1 day (a long time to have tests run)

    def __init__(self,
                 exec_logger,
                 suite,
                 config=None,
                 fixture=None,
                 hooks=None):
        """
        Initializes the TestSuiteExecutor with the test suite to run.
        """
        self.logger = exec_logger

        if _config.SHELL_CONN_STRING is not None:
            # Specifying the shellConnString command line option should override the fixture
            # specified in the YAML configuration to be the no-op fixture.
            self.fixture_config = {"class": fixtures.NOOP_FIXTURE_CLASS}
        else:
            self.fixture_config = fixture

        self.hooks_config = utils.default_if_none(hooks, [])
        self.test_config = utils.default_if_none(config, {})

        self._suite = suite

        # Only start as many jobs as we need. Note this means that the number of jobs we run may not
        # actually be _config.JOBS or self._suite.options.num_jobs.
        jobs_to_start = self._suite.options.num_jobs
        num_tests = len(suite.tests)

        if num_tests < jobs_to_start:
            self.logger.info(
                "Reducing the number of jobs from %d to %d since there are only %d test(s) to run.",
                self._suite.options.num_jobs, num_tests, num_tests)
            jobs_to_start = num_tests

        # Must be done after getting buildlogger configuration.
        self._jobs = [self._make_job(job_num) for job_num in xrange(jobs_to_start)]

    def run(self):
        """
        Executes the test suite.

        Any exceptions that occur during setting up or tearing down a
        fixture are propagated.
        """

        self.logger.info("Starting execution of %ss...", self._suite.test_kind)

        return_code = 0
        teardown_flag = None
        try:
            if not self._setup_fixtures():
                return_code = 2
                return

            num_repeats = self._suite.options.num_repeats
            while num_repeats > 0:
                test_queue = self._make_test_queue()

                partial_reports = [job.report for job in self._jobs]
                self._suite.record_test_start(partial_reports)

                # Have the Job threads destroy their fixture during the final repetition after they
                # finish running their last test. This avoids having a large number of processes
                # still running if an Evergreen task were to time out from a hang/deadlock being
                # triggered.
                teardown_flag = threading.Event() if num_repeats == 1 else None
                (report, interrupted) = self._run_tests(test_queue, teardown_flag)

                self._suite.record_test_end(report)

                # If the user triggered a KeyboardInterrupt, then we should stop.
                if interrupted:
                    raise errors.UserInterrupt("Received interrupt from user")

                if teardown_flag and teardown_flag.is_set():
                    return_code = 2

                sb = []  # String builder.
                self._suite.summarize_latest(sb)
                self.logger.info("Summary: %s", "\n    ".join(sb))

                if not report.wasSuccessful():
                    return_code = 1
                    if self._suite.options.fail_fast:
                        break

                # Clear the report so it can be reused for the next execution.
                for job in self._jobs:
                    job.report.reset()
                num_repeats -= 1
        finally:
            if not teardown_flag:
                if not self._teardown_fixtures():
                    return_code = 2
            self._suite.return_code = return_code

    def _setup_fixtures(self):
        """
        Sets up a fixture for each job.
        """

        # We reset the internal state of the PortAllocator before calling job.fixture.setup() so
        # that ports used by the fixture during a test suite run earlier can be reused during this
        # current test suite.
        network.PortAllocator.reset()

        for job in self._jobs:
            try:
                job.fixture.setup()
            except:
                self.logger.exception(
                    "Encountered an error while setting up %s.", job.fixture)
                return False

        # Once they have all been started, wait for them to become available.
        for job in self._jobs:
            try:
                job.fixture.await_ready()
            except:
                self.logger.exception(
                    "Encountered an error while waiting for %s to be ready", job.fixture)
                return False
        return True

    def _run_tests(self, test_queue, teardown_flag):
        """
        Starts a thread for each Job instance and blocks until all of
        the tests are run.

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
                t = threading.Thread(target=job,
                                     args=(test_queue, interrupt_flag),
                                     kwargs=dict(teardown_flag=teardown_flag))
                # Do not wait for tests to finish executing if interrupted by the user.
                t.daemon = True
                t.start()
                threads.append(t)
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
        else:
            # Only wait for all the Job instances if not interrupted by the user.
            for t in threads:
                t.join()

        reports = [job.report for job in self._jobs]
        combined_report = _report.TestReport.combine(*reports)

        # We cannot return 'interrupt_flag.is_set()' because the interrupt flag can be set by a Job
        # instance if a test fails and it decides to drain the queue. We only want to raise a
        # StopExecution exception in TestSuiteExecutor.run() if the user triggered the interrupt.
        return (combined_report, user_interrupted)

    def _teardown_fixtures(self):
        """
        Tears down all of the fixtures.

        Returns true if all fixtures were torn down successfully, and
        false otherwise.
        """
        success = True
        for job in self._jobs:
            try:
                if not job.fixture.teardown(finished=True):
                    self.logger.warn("Teardown of %s was not successful.", job.fixture)
                    success = False
            except:
                self.logger.exception("Encountered an error while tearing down %s.", job.fixture)
                success = False
        return success

    def _make_fixture(self, job_num, job_logger):
        """
        Creates a fixture for a job.
        """

        fixture_config = {}
        fixture_class = fixtures.NOOP_FIXTURE_CLASS

        if self.fixture_config is not None:
            fixture_config = self.fixture_config.copy()
            fixture_class = fixture_config.pop("class")

        fixture_logger = job_logger.new_fixture_logger(fixture_class)

        return fixtures.make_fixture(fixture_class, fixture_logger, job_num, **fixture_config)

    def _make_hooks(self, job_num, fixture):
        """
        Creates the custom behaviors for the job's fixture.
        """

        behaviors = []

        for behavior_config in self.hooks_config:
            behavior_config = behavior_config.copy()
            behavior_class = behavior_config.pop("class")

            hook_logger = self.logger.new_hook_logger(behavior_class, job_num)
            behavior = _hooks.make_custom_behavior(behavior_class,
                                                   hook_logger,
                                                   fixture,
                                                   **behavior_config)
            behaviors.append(behavior)

        return behaviors

    def _make_job(self, job_num):
        """
        Returns a Job instance with its own fixture, hooks, and test
        report.
        """
        job_logger = self.logger.new_job_logger(self._suite.test_kind, job_num)

        fixture = self._make_fixture(job_num, job_logger)
        hooks = self._make_hooks(job_num, fixture)

        report = _report.TestReport(job_logger, self._suite.options)

        return _job.Job(job_logger, fixture, hooks, report, self._suite.options)

    def _make_test_queue(self):
        """
        Returns a queue of TestCase instances.

        Use a multi-consumer queue instead of a unittest.TestSuite so
        that the test cases can be dispatched to multiple threads.
        """

        test_queue_logger = self.logger.new_testqueue_logger(self._suite.test_kind)
        # Put all the test cases in a queue.
        queue = _queue.Queue()
        for test_name in self._suite.tests:
            test_case = testcases.make_test_case(self._suite.test_kind,
                                                 test_queue_logger,
                                                 test_name,
                                                 **self.test_config)
            queue.put(test_case)

        # Add sentinel value for each job to indicate when there are no more items to process.
        for _ in xrange(len(self._jobs)):
            queue.put(None)

        return queue
