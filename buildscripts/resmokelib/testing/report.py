"""
Extension to the unittest.TestResult to support additional test status
and timing information for the report.json file.
"""

from __future__ import absolute_import

import time
import unittest

from .. import config
from .. import logging


class TestReport(unittest.TestResult):
    """
    Records test status and timing information.
    """

    def __init__(self, logger, logging_config, build_id=None, build_config=None):
        """
        Initializes the TestReport with the buildlogger configuration.
        """

        unittest.TestResult.__init__(self)

        self.logger = logger
        self.logging_config = logging_config
        self.build_id = build_id
        self.build_config = build_config

        self.start_times = {}
        self.end_times = {}
        self.statuses = {}
        self.return_codes = {}
        self.urls = {}

        self.num_succeeded = 0
        self.num_failed = 0
        self.num_errored = 0

        self.__dynamic_tests = set()
        self.__original_loggers = {}

    @classmethod
    def combine(cls, *reports):
        """
        Merges the results from multiple TestReport instances into one.

        If the same test is present in multiple reports, then one that
        failed or errored is more preferred over one that succeeded.
        This behavior is useful for when running multiple jobs that
        dynamically add a #dbhash# test case.
        """

        combined_report = cls(logging.loggers.EXECUTOR, {})
        combining_time = time.time()

        for report in reports:
            if not isinstance(report, TestReport):
                raise TypeError("reports must be a list of TestReport instances")

            for test_id in report.start_times:
                if combined_report.statuses.get(test_id, "pass") != "pass":
                    # 'combined_report' already has a failure recorded for this test, so just keep
                    # the information about that one.
                    continue

                combined_report.start_times[test_id] = report.start_times[test_id]
                combined_report.end_times[test_id] = report.end_times.get(test_id, combining_time)

                # If a StopExecution exception is triggered while running the tests, then it is
                # possible for dynamic tests not to have called TestReport.stopTest() yet.
                if test_id in report.__dynamic_tests:
                    # Mark a dynamic test as having failed if it was interrupted. It might have
                    # passed if the suite ran to completion, but we wouldn't know for sure.
                    combined_report.statuses[test_id] = report.statuses.get(test_id, "fail")
                    combined_report.return_codes[test_id] = report.return_codes.get(test_id, -2)
                else:
                    # A non-dynamic test should always have a status and return code, so it is a
                    # resmoke.py error if it does not.
                    combined_report.statuses[test_id] = report.statuses.get(test_id, "error")
                    combined_report.return_codes[test_id] = report.return_codes.get(test_id, 2)

                if test_id in report.urls:
                    combined_report.urls[test_id] = report.urls[test_id]

            combined_report.__dynamic_tests.update(report.__dynamic_tests)

        # Recompute number of success, failures, and errors.
        combined_report.num_succeeded = len(combined_report.get_successful())
        combined_report.num_failed = len(combined_report.get_failed())
        combined_report.num_errored = len(combined_report.get_errored())

        return combined_report

    def startTest(self, test, dynamic=False):
        """
        Called immediately before 'test' is run.
        """

        unittest.TestResult.startTest(self, test)

        self.start_times[test.id()] = time.time()

        basename = test.basename()
        if dynamic:
            command = "(dynamic test case)"
            self.__dynamic_tests.add(test.id())
        else:
            command = test.as_command()
        self.logger.info("Running %s...\n%s", basename, command)

        test_id = logging.buildlogger.new_test_id(self.build_id,
                                                  self.build_config,
                                                  basename,
                                                  command)

        if self.build_id is not None:
            endpoint = logging.buildlogger.APPEND_TEST_LOGS_ENDPOINT % {
                "build_id": self.build_id,
                "test_id": test_id,
            }

            self.urls[test.id()] = "%s/%s/" % (config.BUILDLOGGER_URL.rstrip("/"),
                                               endpoint.strip("/"))
            self.logger.info("Writing output of %s to %s.",
                             test.shortDescription(), self.urls[test.id()])

        # Set up the test-specific logger.
        logger_name = "%s:%s" % (test.logger.name, test.short_name())
        logger = logging.loggers.new_logger(logger_name, parent=test.logger)
        logging.config.apply_buildlogger_test_handler(logger,
                                                      self.logging_config,
                                                      build_id=self.build_id,
                                                      build_config=self.build_config,
                                                      test_id=test_id)

        self.__original_loggers[test.id()] = test.logger
        test.logger = logger

    def stopTest(self, test):
        """
        Called immediately after 'test' has run.
        """

        unittest.TestResult.stopTest(self, test)
        self.end_times[test.id()] = time.time()

        time_taken = self.end_times[test.id()] - self.start_times[test.id()]
        self.logger.info("%s ran in %0.2f seconds.", test.basename(), time_taken)

        # Asynchronously closes the buildlogger test handler to avoid having too many threads open
        # on 32-bit systems.
        logging.flush.close_later(test.logger)

        # Restore the original logger for the test.
        test.logger = self.__original_loggers.pop(test.id())

    def addError(self, test, err):
        """
        Called when a non-failureException was raised during the
        execution of 'test'.
        """

        unittest.TestResult.addError(self, test, err)
        self.num_errored += 1
        self.statuses[test.id()] = "error"
        self.return_codes[test.id()] = test.return_code

    def setError(self, test):
        """
        Used to change the outcome of an existing test to an error.
        """

        if test.id() not in self.start_times or test.id() not in self.end_times:
            raise ValueError("setError called on a test that has not completed.")

        self.statuses[test.id()] = "error"
        self.return_codes[test.id()] = 2

        # Recompute number of success, failures, and errors.
        self.num_succeeded = len(self.get_successful())
        self.num_failed = len(self.get_failed())
        self.num_errored = len(self.get_errored())

    def addFailure(self, test, err):
        """
        Called when a failureException was raised during the execution
        of 'test'.
        """

        unittest.TestResult.addFailure(self, test, err)
        self.num_failed += 1
        self.statuses[test.id()] = "fail"
        self.return_codes[test.id()] = test.return_code

    def setFailure(self, test, return_code=1):
        """
        Used to change the outcome of an existing test to a failure.
        """

        if test.id() not in self.start_times or test.id() not in self.end_times:
            raise ValueError("setFailure called on a test that has not completed.")

        self.statuses[test.id()] = "fail"
        self.return_codes[test.id()] = return_code

        # Recompute number of success, failures, and errors.
        self.num_succeeded = len(self.get_successful())
        self.num_failed = len(self.get_failed())
        self.num_errored = len(self.get_errored())

    def addSuccess(self, test):
        """
        Called when 'test' executed successfully.
        """

        unittest.TestResult.addSuccess(self, test)
        self.num_succeeded += 1
        self.statuses[test.id()] = "pass"
        self.return_codes[test.id()] = test.return_code

    def wasSuccessful(self):
        """
        Returns true if all tests executed successfully.
        """
        return self.num_failed == self.num_errored == 0

    def num_dynamic(self):
        """
        Returns the number of tests for which startTest(dynamic=True)
        was called.
        """
        return len(self.__dynamic_tests)

    def get_successful(self):
        """
        Returns the ids of the tests that executed successfully.
        """
        return [test_id for test_id in self.statuses if self.statuses[test_id] == "pass"]

    def get_failed(self):
        """
        Returns the ids of the tests that raised a failureException
        during their execution.
        """
        return [test_id for test_id in self.statuses if self.statuses[test_id] == "fail"]

    def get_errored(self):
        """
        Returns the ids of the tests that raised a non-failureException
        during their execution.
        """
        return [test_id for test_id in self.statuses if self.statuses[test_id] == "error"]

    def as_dict(self):
        """
        Return the test result information as a dictionary.

        Used to create the report.json file.
        """

        results = []
        for test_id in self.start_times:
            # Don't distinguish between failures and errors.
            status = "pass" if self.statuses[test_id] == "pass" else "fail"
            start_time = self.start_times[test_id]
            end_time = self.end_times[test_id]

            result = {
                "test_file": test_id,
                "status": status,
                "start": start_time,
                "end": end_time,
                "elapsed": end_time - start_time,
            }

            return_code = self.return_codes[test_id]
            if return_code is not None:
                result["exit_code"] = return_code

            if test_id in self.urls:
                result["url"] = self.urls[test_id]

            results.append(result)

        return {
            "results": results,
            "failures": self.num_failed + self.num_errored,
        }
