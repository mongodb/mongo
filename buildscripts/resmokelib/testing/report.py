"""Extension to the unittest.TestResult.

This is used to support additional test status and timing information for the report.json file.
"""

import copy
import threading
import time
import unittest

from .. import config as _config
from .. import logging


# pylint: disable=attribute-defined-outside-init
class TestReport(unittest.TestResult):  # pylint: disable=too-many-instance-attributes
    """Record test status and timing information."""

    def __init__(self, job_logger, suite_options):
        """Initialize the TestReport with the buildlogger configuration."""

        unittest.TestResult.__init__(self)

        self.job_logger = job_logger
        self.suite_options = suite_options

        self._lock = threading.Lock()

        self.reset()

    @classmethod
    def combine(cls, *reports):
        """Merge the results from multiple TestReport instances into one.

        If the same test is present in multiple reports, then one that
        failed or errored is more preferred over one that succeeded.
        This behavior is useful for when running multiple jobs that
        dynamically add a #dbhash# test case.
        """

        # TestReports that are used when running tests need a JobLogger but combined reports don't
        # use the logger.
        combined_report = cls(logging.loggers.EXECUTOR_LOGGER,
                              _config.SuiteOptions.ALL_INHERITED.resolve())
        combining_time = time.time()

        for report in reports:
            if not isinstance(report, TestReport):
                raise TypeError("reports must be a list of TestReport instances")

            with report._lock:  # pylint: disable=protected-access
                for test_info in report.test_infos:
                    # If the user triggers a KeyboardInterrupt exception while a test is running,
                    # then it is possible for 'test_info' to be modified by a job thread later on.
                    # We make a shallow copy in order to ensure 'num_interrupted' is consistent with
                    # the actual number of tests that have status equal to "timeout".
                    test_info = copy.copy(test_info)

                    # TestReport.addXX() may not have been called.
                    if test_info.status is None or test_info.return_code is None:
                        # Mark the test as having timed out if it was interrupted. It might have
                        # passed if the suite ran to completion, but we wouldn't know for sure.
                        #
                        # Until EVG-1536 is completed, we shouldn't distinguish between failures and
                        # interrupted tests in the report.json file. In Evergreen, the behavior to
                        # sort tests with the "timeout" test status after tests with the "pass" test
                        # status effectively hides interrupted tests from the test results sidebar
                        # unless sorting by the time taken.
                        test_info.status = "timeout"
                        test_info.evergreen_status = "fail"
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
        combined_report.num_interrupted = len(combined_report.get_interrupted())

        return combined_report

    def startTest(self, test):  # pylint: disable=invalid-name
        """Call before 'test' is run."""

        unittest.TestResult.startTest(self, test)

        test_info = _TestInfo(test.id(), test.test_name, test.dynamic)
        test_info.start_time = time.time()

        basename = test.basename()
        command = test.as_command()
        if command:
            self.job_logger.info("Running %s...\n%s", basename, command)
        else:
            self.job_logger.info("Running %s...", basename)

        with self._lock:
            self.test_infos.append(test_info)
            if test.dynamic:
                self.num_dynamic += 1

        # Set up the test-specific logger.
        test_logger = self.job_logger.new_test_logger(test.short_name(), test.basename(), command,
                                                      test.logger)
        test_info.url_endpoint = test_logger.url_endpoint

        test.override_logger(test_logger)

    def stopTest(self, test):  # pylint: disable=invalid-name
        """Call after 'test' has run."""

        unittest.TestResult.stopTest(self, test)

        with self._lock:
            test_info = self.find_test_info(test)
            test_info.end_time = time.time()
            test_status = "no failures detected" if test_info.status == "pass" else "failed"

        time_taken = test_info.end_time - test_info.start_time
        self.job_logger.info("%s ran in %0.2f seconds: %s.", test.basename(), time_taken,
                             test_status)

        # Asynchronously closes the buildlogger test handler to avoid having too many threads open
        # on 32-bit systems.
        for handler in test.logger.handlers:
            # We ignore the cancellation token returned by close_later() since we always want the
            # logs to eventually get flushed.
            logging.flush.close_later(handler)

        # Restore the original logger for the test.
        test.reset_logger()

    def addError(self, test, err):  # pylint: disable=invalid-name
        """Call when a non-failureException was raised during the execution of 'test'."""

        unittest.TestResult.addError(self, test, err)

        with self._lock:
            self.num_errored += 1

            # We don't distinguish between test failures and Python errors in Evergreen.
            test_info = self.find_test_info(test)
            test_info.status = "error"
            test_info.evergreen_status = "fail"
            test_info.return_code = test.return_code

    def setError(self, test):  # pylint: disable=invalid-name
        """Change the outcome of an existing test to an error."""
        self.job_logger.info("setError(%s)", test)

        with self._lock:
            test_info = self.find_test_info(test)
            if test_info.end_time is None:
                raise ValueError("stopTest was not called on %s" % (test.basename()))

            # We don't distinguish between test failures and Python errors in Evergreen.
            test_info.status = "error"
            test_info.evergreen_status = "fail"
            test_info.return_code = 2

        # Recompute number of success, failures, and errors.
        self.num_succeeded = len(self.get_successful())
        self.num_failed = len(self.get_failed())
        self.num_errored = len(self.get_errored())
        self.num_interrupted = len(self.get_interrupted())

    def addFailure(self, test, err):  # pylint: disable=invalid-name
        """Call when a failureException was raised during the execution of 'test'."""

        unittest.TestResult.addFailure(self, test, err)

        with self._lock:
            self.num_failed += 1

            test_info = self.find_test_info(test)
            test_info.status = "fail"
            if test_info.dynamic:
                # Dynamic tests are used for data consistency checks, so the failures are never
                # silenced.
                test_info.evergreen_status = "fail"
            else:
                test_info.evergreen_status = self.suite_options.report_failure_status
            test_info.return_code = test.return_code

    def setFailure(self, test, return_code=1):  # pylint: disable=invalid-name
        """Change the outcome of an existing test to a failure."""

        with self._lock:
            test_info = self.find_test_info(test)
            if test_info.end_time is None:
                raise ValueError("stopTest was not called on %s" % (test.basename()))

            test_info.status = "fail"
            if test_info.dynamic:
                # Dynamic tests are used for data consistency checks, so the failures are never
                # silenced.
                test_info.evergreen_status = "fail"
            else:
                test_info.evergreen_status = self.suite_options.report_failure_status
            test_info.return_code = return_code

        # Recompute number of success, failures, and errors.
        self.num_succeeded = len(self.get_successful())
        self.num_failed = len(self.get_failed())
        self.num_errored = len(self.get_errored())
        self.num_interrupted = len(self.get_interrupted())

    def addSuccess(self, test):  # pylint: disable=invalid-name
        """Call when 'test' executed successfully."""

        unittest.TestResult.addSuccess(self, test)

        with self._lock:
            self.num_succeeded += 1

            test_info = self.find_test_info(test)
            test_info.status = "pass"
            test_info.evergreen_status = "pass"
            test_info.return_code = test.return_code

    def wasSuccessful(self):  # pylint: disable=invalid-name
        """Return true if all tests executed successfully."""

        with self._lock:
            return self.num_failed == self.num_errored == self.num_interrupted == 0

    def get_successful(self):
        """Return the status and timing information of the tests that executed successfully."""

        with self._lock:
            return [test_info for test_info in self.test_infos if test_info.status == "pass"]

    def get_failed(self):
        """Return the status and timing information of tests that raised a failureException."""

        with self._lock:
            return [test_info for test_info in self.test_infos if test_info.status == "fail"]

    def get_errored(self):
        """Return the status and timing information of tests that raised a non-failureException."""

        with self._lock:
            return [test_info for test_info in self.test_infos if test_info.status == "error"]

    def get_interrupted(self):
        """Return the status and timing information of tests that were execution interrupted."""

        with self._lock:
            return [test_info for test_info in self.test_infos if test_info.status == "timeout"]

    def as_dict(self):
        """Return the test result information as a dictionary.

        Used to create the report.json file.
        """

        results = []
        with self._lock:
            for test_info in self.test_infos:
                result = {
                    "test_file": test_info.test_file,
                    "status": test_info.evergreen_status,
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
                "failures": self.num_failed + self.num_errored + self.num_interrupted,
            }

    @classmethod
    def from_dict(cls, report_dict):
        """Return the test report instance copied from a dict (generated in as_dict).

        Used when combining reports instances.
        """

        report = cls(logging.loggers.EXECUTOR_LOGGER, _config.SuiteOptions.ALL_INHERITED.resolve())
        for result in report_dict["results"]:
            # By convention, dynamic tests are named "<basename>:<hook name>".
            is_dynamic = ":" in result["test_file"]
            test_file = result["test_file"]
            # Using test_file as the test id is ok here since the test id only needs to be unique
            # during suite execution.
            test_info = _TestInfo(test_file, test_file, is_dynamic)
            test_info.url_endpoint = result.get("url")
            test_info.status = result["status"]
            test_info.evergreen_status = test_info.status
            test_info.return_code = result["exit_code"]
            test_info.start_time = result["start"]
            test_info.end_time = result["end"]
            report.test_infos.append(test_info)

            if is_dynamic:
                report.num_dynamic += 1

        # Update cached values for number of successful and failed tests.
        report.num_failed = len(report.get_failed())
        report.num_errored = len(report.get_errored())
        report.num_interrupted = len(report.get_interrupted())
        report.num_succeeded = len(report.get_successful())

        return report

    def reset(self):
        """Reset the test report back to its initial state."""

        with self._lock:
            self.test_infos = []

            self.num_dynamic = 0
            self.num_succeeded = 0
            self.num_failed = 0
            self.num_errored = 0
            self.num_interrupted = 0

    def find_test_info(self, test):
        """Return the status and timing information associated with 'test'."""

        test_id = test.id()

        # Search the list backwards to efficiently find the status and timing information of a test
        # that was recently started.
        for test_info in reversed(self.test_infos):
            if test_info.test_id == test_id:
                return test_info

        raise ValueError("Details for %s not found in the report" % (test.basename()))


class _TestInfo(object):  # pylint: disable=too-many-instance-attributes
    """Holder for the test status and timing information."""

    def __init__(self, test_id, test_file, dynamic):
        """Initialize the _TestInfo instance."""

        self.test_id = test_id
        self.test_file = test_file
        self.dynamic = dynamic

        self.start_time = None
        self.end_time = None
        self.status = None
        self.evergreen_status = None
        self.return_code = None
        self.url_endpoint = None
