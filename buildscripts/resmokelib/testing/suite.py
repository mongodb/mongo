"""Holder for the (test kind, list of tests) pair with additional metadata their execution."""

import itertools
import threading
import time

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import selector as _selector
from buildscripts.resmokelib.testing import report as _report
from buildscripts.resmokelib.testing import summary as _summary

# Map of error codes that could be seen. This is collected from:
# * dbshell.cpp
# * exit_code.h
# * Unix signals
# * Windows access violation
EXIT_CODE_MAP = {
    1: "DB Exception",
    -6: "SIGABRT",
    -9: "SIGKILL",
    -11: "SIGSEGV",
    -15: "SIGTERM",
    14: "Exit Abrupt",
    -3: "Failure executing JS file",
    253: "Failure executing JS file",
    -4: "Eval Error",
    252: "Eval Error",
    -5: "Mongorc Error",
    251: "Mongorc Error",
    250: "Unterminated Process",
    -7: "Process Termination Error",
    249: "Process Termination Error",
    -1073741819: "Windows Access Violation",
    3221225477: "Windows Access Violation",
    -1073741571: "Stack Overflow",
    3221225725: "Stack Overflow",
}


def translate_exit_code(exit_code):
    """
    Convert the given exit code into a human readable string.

    :param exit_code: Exit code to translate.
    :return: Human readable string.
    """
    return EXIT_CODE_MAP.get(exit_code, "UNKNOWN")


def synchronized(method):
    """Provide decorator to enforce instance lock ownership when calling the method."""

    def synced(self, *args, **kwargs):
        """Sync an instance lock."""
        lock = getattr(self, "_lock")
        with lock:
            return method(self, *args, **kwargs)

    return synced


class Suite(object):
    """A suite of tests of a particular kind (e.g. C++ unit tests, dbtests, jstests)."""

    def __init__(self, suite_name, suite_config, suite_options=_config.SuiteOptions.ALL_INHERITED):
        """Initialize the suite with the specified name and configuration."""
        self._lock = threading.RLock()

        self._suite_name = suite_name
        self._suite_config = suite_config
        self._suite_options = suite_options

        self.test_kind = self.get_test_kind_config()
        self._tests = None
        self._excluded = None

        self.return_code = None  # Set by the executor.

        self._suite_start_time = None
        self._suite_end_time = None

        self._test_start_times = []
        self._test_end_times = []
        self._reports = []

        # We keep a reference to the TestReports from the currently running jobs so that we can
        # report intermediate results.
        self._partial_reports = None

    def get_config(self):
        """Return the configuration of this suite."""
        return self._suite_config

    def __repr__(self):
        """Create a string representation of object for debugging."""
        return f"{self.test_kind}:{self._suite_name}"

    @property
    def tests(self):
        """Get the tests."""
        if self._tests is None:
            self._tests, self._excluded = self._get_tests_for_kind(self.test_kind)
        return self._tests

    @property
    def excluded(self):
        """Get the excluded."""
        if self._excluded is None:
            self._tests, self._excluded = self._get_tests_for_kind(self.test_kind)
        return self._excluded

    def _get_tests_for_kind(self, test_kind):
        """Return the tests to run based on the 'test_kind'-specific filtering policy."""
        selector_config = self.get_selector_config()

        # The mongos_test doesn't have to filter anything, the selector_config is just the
        # arguments to the mongos program to be used as the test case.
        if test_kind == "mongos_test":
            mongos_options = selector_config  # Just for easier reading.
            if not isinstance(mongos_options, dict):
                raise TypeError("Expected dictionary of arguments to mongos")
            return [mongos_options], []

        return _selector.filter_tests(test_kind, selector_config)

    def get_name(self):
        """Return the name of the test suite."""
        return self._suite_name

    def get_display_name(self):
        """Return the name of the test suite with a unique identifier for its SuiteOptions."""

        if self.options.description is None:
            return self.get_name()

        return "{} ({})".format(self.get_name(), self.options.description)

    def get_selector_config(self):
        """Return the "selector" section of the YAML configuration."""

        if "selector" not in self._suite_config:
            return {}
        selector = self._suite_config["selector"].copy()

        if self.options.include_tags is not None:
            if "include_tags" in selector:
                selector["include_tags"] = {
                    "$allOf": [
                        selector["include_tags"],
                        self.options.include_tags,
                    ]
                }
            elif "exclude_tags" in selector:
                selector["exclude_tags"] = {
                    "$anyOf": [
                        selector["exclude_tags"],
                        {"$not": self.options.include_tags},
                    ]
                }
            else:
                selector["include_tags"] = self.options.include_tags

        return selector

    def get_executor_config(self):
        """Return the "executor" section of the YAML configuration."""
        return self._suite_config["executor"]

    def get_test_kind_config(self):
        """Return the "test_kind" section of the YAML configuration."""
        return self._suite_config["test_kind"]

    def get_num_times_to_repeat_tests(self) -> int:
        """Return the number of times to repeat tests."""
        if self.options.num_repeat_tests:
            return self.options.num_repeat_tests
        return 1

    def get_num_jobs_to_start(self) -> int:
        """
        Determine the number of jobs to start.

        :return: Number of jobs to start.
        """
        num_jobs_to_start = self.options.num_jobs
        num_tests = self._get_num_test_runs()

        if num_tests < num_jobs_to_start:
            num_jobs_to_start = num_tests

        return num_jobs_to_start

    def _get_num_test_runs(self) -> int:
        """Return the number of total test runs."""
        return len(self.tests) * self.options.num_repeat_tests

    @property
    def options(self):
        """Get the options."""
        return self._suite_options.resolve()

    def with_options(self, suite_options):
        """Return a Suite instance with the specified resmokelib.config.SuiteOptions."""

        return Suite(self._suite_name, self._suite_config, suite_options)

    @synchronized
    def record_suite_start(self):
        """Record the start time of the suite."""
        self._suite_start_time = time.time()

    @synchronized
    def record_suite_end(self):
        """Record the end time of the suite."""
        self._suite_end_time = time.time()

    @synchronized
    def record_test_start(self, partial_reports):
        """Record the start time of an execution.

        The result is stored in the TestReports for currently running jobs.
        """
        self._test_start_times.append(time.time())
        self._partial_reports = partial_reports

    @synchronized
    def record_test_end(self, report):
        """Record the end time of an execution."""
        self._test_end_times.append(time.time())
        self._reports.append(report)
        self._partial_reports = None

    @synchronized
    def get_active_report(self):
        """Return the partial report of the currently running execution, if there is one."""
        if not self._partial_reports:
            return None
        return _report.TestReport.combine(*self._partial_reports)

    @synchronized
    def get_reports(self):
        """Return the list of reports.

        If there's an execution currently in progress, then a report for the partial results
        is included in the returned list.
        """

        if self._partial_reports is not None:
            return self._reports + [self.get_active_report()]

        return self._reports

    @synchronized
    def summarize(self, sb):
        """Append a summary of the suite onto the string builder 'sb'."""
        if not self._reports and not self._partial_reports:
            sb.append("No tests ran.")
            summary = _summary.Summary(0, 0.0, 0, 0, 0, 0)
        elif not self._reports and self._partial_reports:
            summary = self.summarize_latest(sb)
        elif len(self._reports) == 1 and not self._partial_reports:
            summary = self._summarize_execution(0, sb)
        else:
            summary = self._summarize_repeated(sb)

        if summary.num_run == 0:
            sb.append("Suite did not run any tests.")
            return

        # Override the 'time_taken' attribute of the summary if we have more accurate timing
        # information available.
        if self._suite_start_time is not None and self._suite_end_time is not None:
            time_taken = self._suite_end_time - self._suite_start_time
            summary = summary._replace(time_taken=time_taken)

    @synchronized
    def summarize_latest(self, sb):
        """Return a summary of the latest execution of the suite.

        Also append a summary of that execution onto the string builder 'sb'.

        If there's an execution currently in progress, then the partial
        summary of that execution is appended to 'sb'.
        """

        if self._partial_reports is None:
            return self._summarize_execution(-1, sb)

        active_report = _report.TestReport.combine(*self._partial_reports)
        # Use the current time as the time that this suite finished running.
        end_time = time.time()
        return self._summarize_report(active_report, self._test_start_times[-1], end_time, sb)

    def _summarize_repeated(self, sb):
        """Return the summary information of all executions.

        Also append each execution's summary onto the string builder 'sb' and
        information of how many repetitions there were.
        """

        reports = self.get_reports()  # Also includes the combined partial reports.
        num_iterations = len(reports)
        start_times = self._test_start_times[:]
        end_times = self._test_end_times[:]
        if self._partial_reports:
            end_times.append(time.time())  # Add an end time in this copy for the partial reports.

        total_time_taken = end_times[-1] - start_times[0]
        sb.append("Executed %d times in %0.2f seconds:" % (num_iterations, total_time_taken))

        combined_summary = _summary.Summary(0, 0.0, 0, 0, 0, 0)
        for iteration in range(num_iterations):
            # Summarize each execution as a bulleted list of results.
            bulleter_sb = []
            summary = self._summarize_report(reports[iteration], start_times[iteration],
                                             end_times[iteration], bulleter_sb)
            combined_summary = _summary.combine(combined_summary, summary)

            for (i, line) in enumerate(bulleter_sb):
                # Only bullet first line, indent others.
                prefix = "* " if i == 0 else "  "
                sb.append(prefix + line)

        return combined_summary

    def _summarize_execution(self, iteration, sb):
        """Return the summary information of the execution given by 'iteration'.

        Also append a summary of that execution onto the string builder 'sb'.
        """

        return self._summarize_report(self._reports[iteration], self._test_start_times[iteration],
                                      self._test_end_times[iteration], sb)

    def _summarize_report(self, report, start_time, end_time, sb):
        """Return the summary information of the execution.

        The summary is for 'report' that started at 'start_time' and finished at 'end_time'.
         Also append a summary of that execution onto the string builder 'sb'.
        """

        time_taken = end_time - start_time

        # Tests that were interrupted are treated as failures because (1) the test has already been
        # started and therefore isn't skipped and (2) the test has yet to finish and therefore
        # cannot be said to have succeeded.
        num_failed = report.num_failed + report.num_interrupted
        num_run = report.num_succeeded + report.num_errored + num_failed
        # The number of skipped tests is only known if self.options.time_repeat_tests_secs
        # is not specified.
        if self.options.time_repeat_tests_secs:
            num_skipped = 0
        else:
            num_tests = len(self.tests) * self.options.num_repeat_tests
            num_skipped = num_tests + report.num_dynamic - num_run

        if report.num_succeeded == num_run and num_skipped == 0:
            sb.append("All %d test(s) passed in %0.2f seconds." % (num_run, time_taken))
            return _summary.Summary(num_run, time_taken, num_run, 0, 0, 0)

        summary = _summary.Summary(num_run, time_taken, report.num_succeeded, num_skipped,
                                   num_failed, report.num_errored)

        sb.append("%d test(s) ran in %0.2f seconds"
                  " (%d succeeded, %d were skipped, %d failed, %d errored)" % summary)

        test_names = []

        if num_failed > 0:
            sb.append("The following tests failed (with exit code):")
            for test_info in itertools.chain(report.get_failed(), report.get_interrupted()):
                test_names.append(test_info.test_file)
                sb.append("    %s (%d %s)" % (test_info.test_file, test_info.return_code,
                                              translate_exit_code(test_info.return_code)))

        if report.num_errored > 0:
            sb.append("The following tests had errors:")
            for test_info in report.get_errored():
                test_names.append(test_info.test_file)
                sb.append("    %s" % (test_info.test_file))

        if num_failed > 0 or report.num_errored > 0:
            test_names.sort(key=_report.test_order)
            sb.append("If you're unsure where to begin investigating these errors, "
                      "consider looking at tests in the following order:")
            for test_name in test_names:
                sb.append("    %s" % (test_name))

        return summary

    @staticmethod
    def log_summaries(logger, suites, time_taken):
        """Log summary of all suites."""
        sb = []
        sb.append(
            "Summary of all suites: %d suites ran in %0.2f seconds" % (len(suites), time_taken))
        for suite in suites:
            suite_sb = []
            suite.summarize(suite_sb)
            sb.append("    %s: %s" % (suite.get_display_name(), "\n    ".join(suite_sb)))

        logger.info("=" * 80)
        logger.info("\n".join(sb))

    def make_test_case_names_list(self):
        """
        Create a list of all the names of the tests.

        :return: List of names of testcases.
        """

        test_case_names = []
        for _ in range(self.get_num_times_to_repeat_tests()):
            for test_name in self.tests:
                test_case_names.append(test_name)
        return test_case_names
