"""
Extension to the unittest.TestResult to support additional test status
and timing information for the report.json file.
"""

from __future__ import absolute_import

import copy
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

        self.reset()

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

            for test_info in report.test_infos:
                # If the user triggers a KeyboardInterrupt exception while a test is running, then
                # it is possible for 'test_info' to be modified by a job thread later on. We make a
                # shallow copy in order to ensure 'num_failed' is consistent with the actual number
                # of tests that have status equal to "failed".
                test_info = copy.copy(test_info)

                # TestReport.addXX() may not have been called.
                if test_info.status is None or test_info.return_code is None:
                    # Mark the test as having failed if it was interrupted. It might have passed if
                    # the suite ran to completion, but we wouldn't know for sure.
                    test_info.status = "fail"
                    test_info.return_code = -2

                # TestReport.stopTest() may not have been called.
                if test_info.end_time is None:
                    # Use the current time as the time that the test finished running.
                    test_info.end_time = combining_time

                combined_report.test_infos.append(test_info)

            combined_report.num_dynamic += report.num_dynamic

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

        test_info = _TestInfo(test.id(), dynamic)
        test_info.start_time = time.time()
        self.test_infos.append(test_info)

        basename = test.basename()
        if dynamic:
            command = "(dynamic test case)"
            self.num_dynamic += 1
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

            test_info.url_endpoint = "%s/%s/" % (config.BUILDLOGGER_URL.rstrip("/"),
                                                 endpoint.strip("/"))

            self.logger.info("Writing output of %s to %s.",
                             test.shortDescription(),
                             test_info.url_endpoint)

        # Set up the test-specific logger.
        logger_name = "%s:%s" % (test.logger.name, test.short_name())
        logger = logging.loggers.new_logger(logger_name, parent=test.logger)
        logging.config.apply_buildlogger_test_handler(logger,
                                                      self.logging_config,
                                                      build_id=self.build_id,
                                                      build_config=self.build_config,
                                                      test_id=test_id)

        self.__original_loggers[test_info.test_id] = test.logger
        test.logger = logger

    def stopTest(self, test):
        """
        Called immediately after 'test' has run.
        """

        unittest.TestResult.stopTest(self, test)

        test_info = self._find_test_info(test)
        test_info.end_time = time.time()

        time_taken = test_info.end_time - test_info.start_time
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

        test_info = self._find_test_info(test)
        test_info.status = "error"
        test_info.return_code = test.return_code

    def setError(self, test):
        """
        Used to change the outcome of an existing test to an error.
        """

        test_info = self._find_test_info(test)
        if test_info.end_time is None:
            raise ValueError("stopTest was not called on %s" % (test.basename()))

        test_info.status = "error"
        test_info.return_code = 2

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

        test_info = self._find_test_info(test)
        test_info.status = "fail"
        test_info.return_code = test.return_code

    def setFailure(self, test, return_code=1):
        """
        Used to change the outcome of an existing test to a failure.
        """

        test_info = self._find_test_info(test)
        if test_info.end_time is None:
            raise ValueError("stopTest was not called on %s" % (test.basename()))

        test_info.status = "fail"
        test_info.return_code = return_code

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

        test_info = self._find_test_info(test)
        test_info.status = "pass"
        test_info.return_code = test.return_code

    def wasSuccessful(self):
        """
        Returns true if all tests executed successfully.
        """
        return self.num_failed == self.num_errored == 0

    def get_successful(self):
        """
        Returns the status and timing information of the tests that
        executed successfully.
        """
        return [test_info for test_info in self.test_infos if test_info.status == "pass"]

    def get_failed(self):
        """
        Returns the status and timing information of the tests that
        raised a failureException during their execution.
        """
        return [test_info for test_info in self.test_infos if test_info.status == "fail"]

    def get_errored(self):
        """
        Returns the status and timing information of the tests that
        raised a non-failureException during their execution.
        """
        return [test_info for test_info in self.test_infos if test_info.status == "error"]

    def as_dict(self):
        """
        Return the test result information as a dictionary.

        Used to create the report.json file.
        """

        results = []
        for test_info in self.test_infos:
            # Don't distinguish between failures and errors.
            status = "pass" if test_info.status == "pass" else "fail"

            result = {
                "test_file": test_info.test_id,
                "status": status,
                "exit_code": test_info.return_code,
                "start": test_info.start_time,
                "end": test_info.end_time,
                "elapsed": test_info.end_time - test_info.start_time,
            }

            if test_info.url_endpoint is not None:
                result["url"] = test_info.url_endpoint
                result["url_raw"] = test_info.url_endpoint + "?raw=1"

            results.append(result)

        return {
            "results": results,
            "failures": self.num_failed + self.num_errored,
        }

    def reset(self):
        """
        Resets the test report back to its initial state.
        """

        self.test_infos = []

        self.num_dynamic = 0
        self.num_succeeded = 0
        self.num_failed = 0
        self.num_errored = 0

        self.__original_loggers = {}

    def _find_test_info(self, test):
        """
        Returns the status and timing information associated with
        'test'.
        """

        test_id = test.id()

        # Search the list backwards to efficiently find the status and timing information of a test
        # that was recently started.
        for test_info in reversed(self.test_infos):
            if test_info.test_id == test_id:
                return test_info

        raise ValueError("Details for %s not found in the report" % (test.basename()))


class _TestInfo(object):
    """
    Holder for the test status and timing information.
    """

    def __init__(self, test_id, dynamic):
        """
        Initializes the _TestInfo instance.
        """

        self.test_id = test_id
        self.dynamic = dynamic

        self.start_time = None
        self.end_time = None
        self.status = None
        self.return_code = None
        self.url_endpoint = None
