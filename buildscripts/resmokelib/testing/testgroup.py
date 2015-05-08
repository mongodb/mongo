"""
Holder for the (test kind, list of tests) pair with additional metadata
about when and how they execute.
"""

from __future__ import absolute_import

import time

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

    def get_latest_report(self):
        """
        Returns the report of the most recent execution, and None if
        the test group has not been executed.
        """
        if self._reports:
            return self._reports[-1]
        return None

    def record_start(self):
        """
        Records the start time of an execution.
        """
        self._start_times.append(time.time())

    def record_end(self, report):
        """
        Records the end time of an execution.
        """
        self._end_times.append(time.time())
        self._reports.append(report)

    def summarize(self, sb):
        """
        Appends a summary of the latest execution onto the string
        builder 'sb'.

        TODO: summarize more than just the most recent report
        """

        if not self._reports:
            sb.append("No tests ran.")
            return _summary.Summary(0, 0.0, 0, 0, 0, 0)

        report = self._reports[-1]
        time_taken = self._end_times[-1] - self._start_times[-1]

        num_run = report.num_succeeded + report.num_errored + report.num_failed
        num_skipped = len(self.tests) + report.num_dynamic() - num_run

        if report.num_succeeded == num_run and num_skipped == 0:
            sb.append("All %d test(s) passed in %0.2f seconds." % (num_run, time_taken))
            return _summary.Summary(num_run, time_taken, num_run, 0, 0, 0)

        summary = _summary.Summary(num_run, time_taken, report.num_succeeded, num_skipped,
                                   report.num_failed, report.num_errored)

        sb.append("%d test(s) ran in %0.2f seconds"
                  " (%d succeeded, %d were skipped, %d failed, %d errored)" % summary)

        if report.num_failed > 0:
            sb.append("The following tests failed (with exit code):")
            for test_id in report.get_failed():
                sb.append("    %s (%d)" % (test_id, report.return_codes[test_id]))

        if report.num_errored > 0:
            sb.append("The following tests had errors:")
            for test_id in report.get_errored():
                sb.append("    %s" % test_id)

        return summary
