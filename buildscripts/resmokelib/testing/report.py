# pylint: disable=invalid-name
"""Extension to the unittest.TestResult.

This is used to support additional test status and timing information for the report.json file.
"""

import copy
import threading
import time
import unittest

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import logging
from buildscripts.resmokelib.testing.symbolizer_service import ResmokeSymbolizer


# pylint: disable=attribute-defined-outside-init
class TestReport(unittest.TestResult):
    """Record test status and timing information."""

    def __init__(self, job_logger, suite_options, job_num=None):
        """
        Initialize the TestReport with the buildlogger configuration.

        :param job_logger: The higher-level logger that will be used to print metadata about the test.
        :param suite_options: Options for the suite being executed.
        :param job_num: The number corresponding to the job this test runs in.
        """

        unittest.TestResult.__init__(self)

        self.job_logger = job_logger
        self.job_num = job_num
        self.suite_options = suite_options
        self.logging_prefix = None

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
        combined_report = cls(logging.loggers.ROOT_EXECUTOR_LOGGER,
                              _config.SuiteOptions.ALL_INHERITED.resolve())
        combining_time = time.time()

        for report in reports:
            if not isinstance(report, TestReport):
                raise TypeError(
                    f"reports must be a list of TestReport instances, current report is {type(report)}"
                )

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

                    # If we receive a SIGUSR1 then we may start combining reports before
                    # their start time has been set.
                    if test_info.start_time is None:
                        test_info.start_time = combining_time

                    combined_report.test_infos.append(test_info)

                combined_report.num_dynamic += report.num_dynamic

        # Recompute number of success, failures, and errors.
        combined_report.num_succeeded = len(combined_report.get_successful())
        combined_report.num_failed = len(combined_report.get_failed())
        combined_report.num_errored = len(combined_report.get_errored())
        combined_report.num_interrupted = len(combined_report.get_interrupted())

        return combined_report

    def startTest(self, test):
        """Call before 'test' is run."""

        unittest.TestResult.startTest(self, test)

        test_info = TestInfo(test.id(), test.test_name, test.dynamic)

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
        (test_logger, url_endpoint) = logging.loggers.new_test_logger(test.short_name(),
                                                                      test.basename(), command,
                                                                      test.logger, self.job_num,
                                                                      test.id(), self.job_logger)
        test_info.url_endpoint = url_endpoint
        if self.logging_prefix is not None:
            test_logger.info(self.logging_prefix)
        # Set job_num in test.
        test.job_num = self.job_num

        test.override_logger(test_logger)
        test_info.start_time = time.time()

    def stopTest(self, test):
        """Call after 'test' has run."""

        # check if there are stacktrace files, if so, invoke the symbolizer here.
        # If there are no stacktrace files for this job, we do not need to invoke the symbolizer at all.
        # Take a lock to download the debug symbols if it hasn't already been downloaded.
        # log symbolized output to test.logger.info()

        symbolizer = ResmokeSymbolizer()
        symbolizer.symbolize_test_logs(test)
        # symbolization completed

        unittest.TestResult.stopTest(self, test)

        failed_test_status = "failed"

        with self._lock:
            test_info = self.find_test_info(test)
            test_info.end_time = time.time()
            test_status = "no failures detected" if test_info.status == "pass" else failed_test_status

        time_taken = test_info.end_time - test_info.start_time
        self.job_logger.info("%s ran in %0.2f seconds: %s.", test.basename(), time_taken,
                             test_status)

        # Asynchronously closes the buildlogger test handler to avoid having too many threads open
        # on 32-bit systems.
        for handler in test.logger.handlers:
            # We ignore the cancellation token returned by close_later() since we always want the
            # logs to eventually get flushed.
            logging.flush.close_later(handler, test_failed_flag=(test_status == failed_test_status))

        # Restore the original logger for the test.
        test.reset_logger()

    def addError(self, test, err):
        """Call when a non-failureException was raised during the execution of 'test'."""

        unittest.TestResult.addError(self, test, err)

        with self._lock:
            self.num_errored += 1

            # We don't distinguish between test failures and Python errors in Evergreen.
            test_info = self.find_test_info(test)
            test_info.status = "error"
            test_info.evergreen_status = "fail"
            test_info.return_code = test.return_code

    def setError(self, test):
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

    def addFailure(self, test, err):
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

    def setFailure(self, test, return_code=1):
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

    def addSuccess(self, test):
        """Call when 'test' executed successfully."""

        unittest.TestResult.addSuccess(self, test)

        with self._lock:
            self.num_succeeded += 1

            test_info = self.find_test_info(test)
            test_info.status = "pass"
            test_info.evergreen_status = "pass"
            test_info.return_code = test.return_code

    def wasSuccessful(self):
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

                if test_info.display_test_name is not None:
                    result["display_test_name"] = test_info.display_test_name

                if test_info.group_id is not None:
                    result["group_id"] = test_info.group_id

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

        report = cls(logging.loggers.ROOT_EXECUTOR_LOGGER,
                     _config.SuiteOptions.ALL_INHERITED.resolve())
        for result in report_dict["results"]:
            # By convention, dynamic tests are named "<basename>:<hook name>".
            is_dynamic = ":" in result["test_file"] or ":" in result.get("display_test_name", "")
            test_file = result["test_file"]
            # Using test_file as the test id is ok here since the test id only needs to be unique
            # during suite execution.
            test_info = TestInfo(test_file, test_file, is_dynamic)
            test_info.display_test_name = result.get("display_test_name")
            test_info.group_id = result.get("group_id")
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


class TestInfo(object):
    """Holder for the test status and timing information."""

    def __init__(self, test_id, test_file, dynamic):
        """Initialize the TestInfo instance."""

        self.test_id = test_id
        self.test_file = test_file
        self.display_test_name = None
        self.dynamic = dynamic

        self.group_id = None
        self.start_time = None
        self.end_time = None
        self.status = None
        self.evergreen_status = None
        self.return_code = None
        self.url_endpoint = None


def test_order(test_name):
    """
    A key function used for sorting TestInfo objects by recommended order of investigation.

    Investigate setup/teardown errors, then hooks, then test files.
    """

    if 'fixture_setup' in test_name:
        return 1
    elif 'fixture_teardown' in test_name:
        return 2
    elif 'fixture_abort' in test_name:
        return 3
    elif ':' in test_name:
        return 4
    else:
        return 5
