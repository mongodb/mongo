import unittest

import buildscripts.monitor_build_status.cli as under_test


class TestSummarize(unittest.TestCase):
    def test_previous_unknown_current_red_not_friday(self):
        previous_build_status = under_test.BuildStatus.UNKNOWN
        current_build_status = under_test.BuildStatus.RED
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.YELLOW
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED_X2.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_YELLOW.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_green_current_red_not_friday(self):
        previous_build_status = under_test.BuildStatus.GREEN
        current_build_status = under_test.BuildStatus.RED
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.YELLOW
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED_X2.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_YELLOW.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_yellow_current_red_not_friday(self):
        previous_build_status = under_test.BuildStatus.YELLOW
        current_build_status = under_test.BuildStatus.RED
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.YELLOW
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_IS.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED_X2.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_YELLOW.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_red_current_red_not_friday(self):
        previous_build_status = under_test.BuildStatus.RED
        current_build_status = under_test.BuildStatus.RED
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.RED
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_IS.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED_X2.value}\n"
            f"{under_test.SummaryMsg.STILL_RED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_RED.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_unknown_current_red_on_friday(self):
        previous_build_status = under_test.BuildStatus.UNKNOWN
        current_build_status = under_test.BuildStatus.RED
        today_is_friday = True

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.RED
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED_X2.value}\n"
            f"{under_test.SummaryMsg.ENTER_RED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_RED.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_green_current_red_on_friday(self):
        previous_build_status = under_test.BuildStatus.GREEN
        current_build_status = under_test.BuildStatus.RED
        today_is_friday = True

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.RED
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED_X2.value}\n"
            f"{under_test.SummaryMsg.ENTER_RED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_RED.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_yellow_current_red_on_friday(self):
        previous_build_status = under_test.BuildStatus.YELLOW
        current_build_status = under_test.BuildStatus.RED
        today_is_friday = True

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.RED
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED_X2.value}\n"
            f"{under_test.SummaryMsg.ENTER_RED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_RED.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_red_current_red_on_friday(self):
        previous_build_status = under_test.BuildStatus.RED
        current_build_status = under_test.BuildStatus.RED
        today_is_friday = True

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.RED
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_IS.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED_X2.value}\n"
            f"{under_test.SummaryMsg.STILL_RED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_RED.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_unknown_current_yellow(self):
        previous_build_status = under_test.BuildStatus.UNKNOWN
        current_build_status = under_test.BuildStatus.YELLOW
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.YELLOW
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_YELLOW.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_green_current_yellow(self):
        previous_build_status = under_test.BuildStatus.GREEN
        current_build_status = under_test.BuildStatus.YELLOW
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.YELLOW
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_YELLOW.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_yellow_current_yellow(self):
        previous_build_status = under_test.BuildStatus.YELLOW
        current_build_status = under_test.BuildStatus.YELLOW
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.YELLOW
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_IS.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_YELLOW.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_red_current_yellow(self):
        previous_build_status = under_test.BuildStatus.RED
        current_build_status = under_test.BuildStatus.YELLOW
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.RED
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_IS.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED.value}\n"
            f"{under_test.SummaryMsg.STILL_RED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_RED.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_unknown_current_green(self):
        previous_build_status = under_test.BuildStatus.UNKNOWN
        current_build_status = under_test.BuildStatus.GREEN
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.GREEN
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_GREEN.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_green_current_green(self):
        previous_build_status = under_test.BuildStatus.GREEN
        current_build_status = under_test.BuildStatus.GREEN
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.GREEN
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_IS.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_GREEN.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_yellow_current_green(self):
        previous_build_status = under_test.BuildStatus.YELLOW
        current_build_status = under_test.BuildStatus.GREEN
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.GREEN
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_GREEN.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)

    def test_previous_red_current_green(self):
        previous_build_status = under_test.BuildStatus.RED
        current_build_status = under_test.BuildStatus.GREEN
        today_is_friday = False

        resulting_build_status, summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            previous_build_status, current_build_status, today_is_friday
        )

        expected_status = under_test.BuildStatus.GREEN
        expected_summary = (
            f"{under_test.SummaryMsg.TITLE.value}\n"
            f"{under_test.SummaryMsg.STATUS_CHANGED.value.format(status=expected_status.value)}\n"
            f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}\n"
            f"{under_test.SummaryMsg.EXIT_RED.value}\n"
            f"{under_test.SummaryMsg.ACTION_ON_GREEN.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(resulting_build_status, expected_status)
        self.assertEqual(summary, expected_summary)
