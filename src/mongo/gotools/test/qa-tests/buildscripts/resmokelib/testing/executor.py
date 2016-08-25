"""
Driver of the test execution framework.
"""

from __future__ import absolute_import

import threading

from . import fixtures
from . import hooks as _hooks
from . import job as _job
from . import report as _report
from . import testcases
from .. import config as _config
from .. import errors
from .. import logging
from .. import utils
from ..utils import queue as _queue


class TestGroupExecutor(object):
    """
    Executes a test group.

    Responsible for setting up and tearing down the fixtures that the
    tests execute against.
    """

    _TIMEOUT = 24 * 60 * 60  # =1 day (a long time to have tests run)

    def __init__(self,
                 exec_logger,
                 test_group,
                 logging_config,
                 config=None,
                 fixture=None,
                 hooks=None):
        """
        Initializes the TestGroupExecutor with the test group to run.
        """

        # Build a logger for executing this group of tests.
        logger_name = "%s:%s" % (exec_logger.name, test_group.test_kind)
        self.logger = logging.loggers.new_logger(logger_name, parent=exec_logger)

        self.logging_config = logging_config
        self.fixture_config = fixture
        self.hooks_config = utils.default_if_none(hooks, [])
        self.test_config = utils.default_if_none(config, {})

        self._test_group = test_group

        self._using_buildlogger = logging.config.using_buildlogger(logging_config)
        self._build_config = None

        if self._using_buildlogger:
            self._build_config = logging.buildlogger.get_config()

        # Must be done after getting buildlogger configuration.
        self._jobs = [self._make_job(job_num) for job_num in xrange(_config.JOBS)]

    def run(self):
        """
        Executes the test group.

        Any exceptions that occur during setting up or tearing down a
        fixture are propagated.
        """

        self.logger.info("Starting execution of %ss...", self._test_group.test_kind)

        return_code = 0
        try:
            if not self._setup_fixtures():
                return_code = 2
                return

            num_repeats = _config.REPEAT
            while num_repeats > 0:
                test_queue = self._make_test_queue()
                self._test_group.record_start()
                (report, interrupted) = self._run_tests(test_queue)
                self._test_group.record_end(report)

                # If the user triggered a KeyboardInterrupt, then we should stop.
                if interrupted:
                    raise errors.UserInterrupt("Received interrupt from user")

                sb = []  # String builder.
                self._test_group.summarize_latest(sb)
                self.logger.info("Summary: %s", "\n    ".join(sb))

                if not report.wasSuccessful():
                    return_code = 1
                    if _config.FAIL_FAST:
                        break

                # Clear the report so it can be reused for the next execution.
                for job in self._jobs:
                    job.report.reset()
                num_repeats -= 1
        finally:
            if not self._teardown_fixtures():
                return_code = 2
            self._test_group.return_code = return_code

    def _setup_fixtures(self):
        """
        Sets up a fixture for each job.
        """

        for job in self._jobs:
            try:
                job.fixture.setup()
            except:
                self.logger.exception("Encountered an error while setting up %s.", job.fixture)
                return False

        # Once they have all been started, wait for them to become available.
        for job in self._jobs:
            try:
                job.fixture.await_ready()
            except:
                self.logger.exception("Encountered an error while waiting for %s to be ready",
                                      job.fixture)
                return False

        return True

    def _run_tests(self, test_queue):
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
                t = threading.Thread(target=job, args=(test_queue, interrupt_flag))
                # Do not wait for tests to finish executing if interrupted by the user.
                t.daemon = True
                t.start()
                threads.append(t)

            joined = False
            while not joined:
                # Need to pass a timeout to join() so that KeyboardInterrupt exceptions
                # are propagated.
                joined = test_queue.join(TestGroupExecutor._TIMEOUT)
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
        # StopExecution exception in TestGroupExecutor.run() if the user triggered the interrupt.
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
                if not job.fixture.teardown():
                    self.logger.warn("Teardown of %s was not successful.", job.fixture)
                    success = False
            except:
                self.logger.exception("Encountered an error while tearing down %s.", job.fixture)
                success = False

        return success

    def _get_build_id(self, job_num):
        """
        Returns a unique build id for a job.
        """

        build_config = self._build_config

        if self._using_buildlogger:
            # Use a distinct "builder" for each job in order to separate their logs.
            if build_config is not None and "builder" in build_config:
                build_config = build_config.copy()
                build_config["builder"] = "%s_job%d" % (build_config["builder"], job_num)

            build_id = logging.buildlogger.new_build_id(build_config)

            if build_config is None or build_id is None:
                self.logger.info("Encountered an error configuring buildlogger for job #%d, falling"
                                 " back to stderr.", job_num)

            return build_id, build_config

        return None, build_config

    def _make_fixture(self, job_num, build_id, build_config):
        """
        Creates a fixture for a job.
        """

        fixture_config = {}
        fixture_class = fixtures.NOOP_FIXTURE_CLASS

        if self.fixture_config is not None:
            fixture_config = self.fixture_config.copy()
            fixture_class = fixture_config.pop("class")

        logger_name = "%s:job%d" % (fixture_class, job_num)
        logger = logging.loggers.new_logger(logger_name, parent=logging.loggers.FIXTURE)
        logging.config.apply_buildlogger_global_handler(logger,
                                                        self.logging_config,
                                                        build_id=build_id,
                                                        build_config=build_config)

        return fixtures.make_fixture(fixture_class, logger, job_num, **fixture_config)

    def _make_hooks(self, job_num, fixture):
        """
        Creates the custom behaviors for the job's fixture.
        """

        behaviors = []

        for behavior_config in self.hooks_config:
            behavior_config = behavior_config.copy()
            behavior_class = behavior_config.pop("class")

            logger_name = "%s:job%d" % (behavior_class, job_num)
            logger = logging.loggers.new_logger(logger_name, parent=self.logger)
            behavior = _hooks.make_custom_behavior(behavior_class,
                                                   logger,
                                                   fixture,
                                                   **behavior_config)
            behaviors.append(behavior)

        return behaviors

    def _make_job(self, job_num):
        """
        Returns a Job instance with its own fixture, hooks, and test
        report.
        """

        build_id, build_config = self._get_build_id(job_num)
        fixture = self._make_fixture(job_num, build_id, build_config)
        hooks = self._make_hooks(job_num, fixture)

        logger_name = "%s:job%d" % (self.logger.name, job_num)
        logger = logging.loggers.new_logger(logger_name, parent=self.logger)

        if build_id is not None:
            endpoint = logging.buildlogger.APPEND_GLOBAL_LOGS_ENDPOINT % {"build_id": build_id}
            url = "%s/%s/" % (_config.BUILDLOGGER_URL.rstrip("/"), endpoint.strip("/"))
            logger.info("Writing output of job #%d to %s.", job_num, url)

        report = _report.TestReport(logger,
                                    self.logging_config,
                                    build_id=build_id,
                                    build_config=build_config)

        return _job.Job(logger, fixture, hooks, report)

    def _make_test_queue(self):
        """
        Returns a queue of TestCase instances.

        Use a multi-consumer queue instead of a unittest.TestSuite so
        that the test cases can be dispatched to multiple threads.
        """

        test_kind_logger = logging.loggers.new_logger(self._test_group.test_kind,
                                                      parent=logging.loggers.TESTS)

        # Put all the test cases in a queue.
        queue = _queue.Queue()
        for test_name in self._test_group.tests:
            test_case = testcases.make_test_case(self._test_group.test_kind,
                                                 test_kind_logger,
                                                 test_name,
                                                 **self.test_config)
            queue.put(test_case)

        # Add sentinel value for each job to indicate when there are no more items to process.
        for _ in xrange(_config.JOBS):
            queue.put(None)

        return queue
