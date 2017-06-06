"""
Holder for the (test kind, list of tests) pair with additional metadata about when and how they
execute.
"""

from __future__ import absolute_import

import itertools
import time

from . import report as _report
from . import summary as _summary
from .. import config as _config
from .. import selector as _selector


class Suite(object):
    """
    A suite of tests of a particular kind (e.g. C++ unit tests, dbtests, jstests).
    """

    def __init__(self, suite_name, suite_config):
        """
        Initializes the suite with the specified name and configuration.
        """

        self._suite_name = suite_name
        self._suite_config = suite_config

        self.test_kind = self.get_test_kind_config()
        self.tests = self._get_tests_for_kind(self.test_kind)

        self.return_code = None  # Set by the executor.

        self._suite_start_time = None
        self._suite_end_time = None

        self._test_start_times = []
        self._test_end_times = []
        self._reports = []

        # We keep a reference to the TestReports from the currently running jobs so that we can
        # report intermediate results.
        self._partial_reports = None

    def _get_tests_for_kind(self, test_kind):
        """
        Returns the tests to run based on the 'test_kind'-specific
        filtering policy.
        """

        test_info = self.get_selector_config()

        # The mongos_test doesn't have to filter anything, the test_info is just the arguments to
        # the mongos program to be used as the test case.
        if test_kind == "mongos_test":
            mongos_options = test_info  # Just for easier reading.
            if not isinstance(mongos_options, dict):
                raise TypeError("Expected dictionary of arguments to mongos")
            return [mongos_options]
        elif test_kind == "cpp_integration_test":
            tests = _selector.filter_cpp_integration_tests(**test_info)
        elif test_kind == "cpp_unit_test":
            tests = _selector.filter_cpp_unit_tests(**test_info)
        elif test_kind == "db_test":
            tests = _selector.filter_dbtests(**test_info)
        else:  # test_kind == "js_test":
            tests = _selector.filter_jstests(**test_info)

        if _config.ORDER_TESTS_BY_NAME:
            return sorted(tests, key=str.lower)
        return tests

    def get_name(self):
        """
        Returns the name of the test suite.
        """
        return self._suite_name

    def get_selector_config(self):
        """
        Returns the "selector" section of the YAML configuration.
        """
        return self._suite_config["selector"]

    def get_executor_config(self):
        """
        Returns the "executor" section of the YAML configuration.
        """
        return self._suite_config["executor"]

    def get_test_kind_config(self):
        """
        Returns the "test_kind" section of the YAML configuration.
        """
        return self._suite_config["test_kind"]

    def record_suite_start(self):
        """
        Records the start time of the suite.
        """
        self._suite_start_time = time.time()

    def record_suite_end(self):
        """
        Records the end time of the suite.
        """
        self._suite_end_time = time.time()

    def record_test_start(self, partial_reports):
        """
        Records the start time of an execution and stores the
        TestReports for currently running jobs.
        """
        self._test_start_times.append(time.time())
        self._partial_reports = partial_reports

    def record_test_end(self, report):
        """
        Records the end time of an execution.
        """
        self._test_end_times.append(time.time())
        self._reports.append(report)
        self._partial_reports = None

    def interrupt(self):
        """
        Records the end of the suite and forces the end of the execution's report.

        Used when handling SIGUSR1 interrupts.
        """

        if self._suite_end_time:
            return

        self.record_suite_end()

        #  Converts any partial reports to completed reports and ensures that report is ended.
        active_report = self.get_active_report()
        if active_report:
            self.record_test_end(active_report)

    def get_active_report(self):
        """
        Returns the partial report of the currently running execution, if there is one.
        """
        if not self._partial_reports:
            return None
        return _report.TestReport.combine(*self._partial_reports)

    def get_reports(self):
        """
        Returns the list of reports. If there's an execution currently
        in progress, then a report for the partial results is included
        in the returned list.
        """

        if self._partial_reports is not None:
            return self._reports + [self.get_active_report()]

        return self._reports

    def summarize(self, sb):
        """
        Appends a summary of the suite onto the string builder 'sb'.
        """

        if not self._reports:
            sb.append("No tests ran.")
            summary = _summary.Summary(0, 0.0, 0, 0, 0, 0)
        elif len(self._reports) == 1:
            summary = self._summarize_execution(0, sb)
        else:
            summary = self._summarize_repeated(sb)

        summarized_group = "    %ss: %s" % (self.test_kind, "\n        ".join(sb))

        if summary.num_run == 0:
            sb.append("Suite did not run any tests.")
            return

        # Override the 'time_taken' attribute of the summary if we have more accurate timing
        # information available.
        if self._suite_start_time is not None and self._suite_end_time is not None:
            time_taken = self._suite_end_time - self._suite_start_time
            summary = summary._replace(time_taken=time_taken)

        sb.append("%d test(s) ran in %0.2f seconds"
                  " (%d succeeded, %d were skipped, %d failed, %d errored)" % summary)

        sb.append(summarized_group)

    def summarize_latest(self, sb):
        """
        Returns a summary of the latest execution of the suite and appends a
        summary of that execution onto the string builder 'sb'.

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
        """
        Returns the summary information of all executions and appends
        each execution's summary onto the string builder 'sb'. Also
        appends information of how many repetitions there were.
        """

        num_iterations = len(self._reports)
        total_time_taken = self._test_end_times[-1] - self._test_start_times[0]
        sb.append("Executed %d times in %0.2f seconds:" % (num_iterations, total_time_taken))

        combined_summary = _summary.Summary(0, 0.0, 0, 0, 0, 0)
        for iteration in xrange(num_iterations):
            # Summarize each execution as a bulleted list of results.
            bulleter_sb = []
            summary = self._summarize_execution(iteration, bulleter_sb)
            combined_summary = _summary.combine(combined_summary, summary)

            for (i, line) in enumerate(bulleter_sb):
                # Only bullet first line, indent others.
                prefix = "* " if i == 0 else "  "
                sb.append(prefix + line)

        return combined_summary

    def _summarize_execution(self, iteration, sb):
        """
        Returns the summary information of the execution given by
        'iteration' and appends a summary of that execution onto the
        string builder 'sb'.
        """

        return self._summarize_report(self._reports[iteration],
                                      self._test_start_times[iteration],
                                      self._test_end_times[iteration],
                                      sb)

    def _summarize_report(self, report, start_time, end_time, sb):
        """
        Returns the summary information of the execution given by
        'report' that started at 'start_time' and finished at
        'end_time', and appends a summary of that execution onto the
        string builder 'sb'.
        """

        time_taken = end_time - start_time

        # Tests that were interrupted are treated as failures because (1) the test has already been
        # started and therefore isn't skipped and (2) the test has yet to finish and therefore
        # cannot be said to have succeeded.
        num_failed = report.num_failed + report.num_interrupted
        num_run = report.num_succeeded + report.num_errored + num_failed
        num_skipped = len(self.tests) + report.num_dynamic - num_run

        if report.num_succeeded == num_run and num_skipped == 0:
            sb.append("All %d test(s) passed in %0.2f seconds." % (num_run, time_taken))
            return _summary.Summary(num_run, time_taken, num_run, 0, 0, 0)

        summary = _summary.Summary(num_run, time_taken, report.num_succeeded, num_skipped,
                                   num_failed, report.num_errored)

        sb.append("%d test(s) ran in %0.2f seconds"
                  " (%d succeeded, %d were skipped, %d failed, %d errored)" % summary)

        if num_failed > 0:
            sb.append("The following tests failed (with exit code):")
            for test_info in itertools.chain(report.get_failed(), report.get_interrupted()):
                sb.append("    %s (%d)" % (test_info.test_id, test_info.return_code))

        if report.num_errored > 0:
            sb.append("The following tests had errors:")
            for test_info in report.get_errored():
                sb.append("    %s" % (test_info.test_id))

        return summary

    @staticmethod
    def log_summaries(logger, suites, time_taken):
        sb = []
        sb.append("Summary of all suites: %d suites ran in %0.2f seconds"
                  % (len(suites), time_taken))
        for suite in suites:
            suite_sb = []
            suite.summarize(suite_sb)
            sb.append("    %s: %s" % (suite.get_name(), "\n    ".join(suite_sb)))

        logger.info("=" * 80)
        logger.info("\n".join(sb))
