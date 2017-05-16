"""
Holder for the (test kind, list of tests) pair with additional metadata
about when and how they execute.
"""

from __future__ import absolute_import

import itertools
import time

from . import report as _report
from . import summary as _summary


class TestGroup(object):
    """
    A class to encapsulate the results of running a group of tests
    of a particular kind (e.g. C++ unit tests, dbtests, jstests).
    """

    def __init__(self, test_kind, tests):
        """
        Initializes the TestGroup with a list of tests.
        """

        self.test_kind = test_kind
        self.tests = tests

        self.return_code = None  # Set by the executor.

        self._start_times = []
        self._end_times = []
        self._reports = []

        # We keep a reference to the TestReports from the currently running jobs so that we can
        # report intermediate results.
        self._partial_reports = None

    def get_reports(self):
        """
        Returns the list of reports. If there's an execution currently
        in progress, then a report for the partial results is included
        in the returned list.
        """

        if self._partial_reports is not None:
            active_report = _report.TestReport.combine(*self._partial_reports)
            return self._reports + [active_report]

        return self._reports

    def record_start(self, partial_reports):
        """
        Records the start time of an execution and stores the
        TestReports for currently running jobs.
        """
        self._start_times.append(time.time())
        self._partial_reports = partial_reports

    def record_end(self, report):
        """
        Records the end time of an execution.
        """
        self._end_times.append(time.time())
        self._reports.append(report)
        self._partial_reports = None

    def summarize_latest(self, sb):
        """
        Returns a summary of the latest execution of the group and appends a
        summary of that execution onto the string builder 'sb'.

        If there's an execution currently in progress, then the partial
        summary of that execution is appended to 'sb'.
        """

        if self._partial_reports is None:
            return self._summarize_execution(-1, sb)

        active_report = _report.TestReport.combine(*self._partial_reports)
        # Use the current time as the time that the test group finished running.
        end_time = time.time()
        return self._summarize_report(active_report, self._start_times[-1], end_time, sb)

    def summarize(self, sb):
        """
        Returns a summary of the execution(s) of the group and appends a
        summary of the execution(s) onto the string builder 'sb'.
        """

        if not self._reports:
            sb.append("No tests ran.")
            return _summary.Summary(0, 0.0, 0, 0, 0, 0)

        if len(self._reports) == 1:
            return self._summarize_execution(0, sb)

        return self._summarize_repeated(sb)

    def _summarize_repeated(self, sb):
        """
        Returns the summary information of all executions and appends
        each execution's summary onto the string builder 'sb'. Also
        appends information of how many repetitions there were.
        """

        num_iterations = len(self._reports)
        total_time_taken = self._end_times[-1] - self._start_times[0]
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
                                      self._start_times[iteration],
                                      self._end_times[iteration],
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
