import unittest

import buildscripts.monitor_build_status.cli as under_test


class TestSummarize(unittest.TestCase):
    def test_all_thresholds_below_100(self):
        scope_percentages = {
            "Scope 1": [0.0, 0.0, 0.0],
            "Scope 2": [0.0, 0.0, 0.0],
            "Scope 3": [0.0, 0.0, 0.0],
            "Scope 4": [0.0, 0.0, 0.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, set()
        )

        expected_summary = f"`SUMMARY [scope]` " f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}"

        self.assertEqual(summary, expected_summary)

    def test_all_thresholds_are_100(self):
        scope_percentages = {
            "Scope 1": [100.0, 100.0, 100.0],
            "Scope 2": [100.0, 100.0, 100.0],
            "Scope 3": [100.0, 100.0, 100.0],
            "Scope 4": [100.0, 100.0, 100.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, set()
        )

        expected_summary = f"`SUMMARY [scope]` " f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}"

        self.assertEqual(summary, expected_summary)

    def test_some_threshold_exceeded_100(self):
        scope_percentages = {
            "Scope 1": [101.0, 0.0, 0.0],
            "Scope 2": [0.0, 101.0, 0.0],
            "Scope 3": [0.0, 0.0, 101.0],
            "Scope 4": [0.0, 0.0, 0.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, set()
        )

        expected_summary = (
            f"`SUMMARY [scope]` "
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED.value}\n"
            f"\t- Scope 1\n\t- Scope 2\n\t- Scope 3"
        )

        self.assertEqual(summary, expected_summary)

    def test_zero_quota_violated_only(self):
        scope_percentages = {
            "Scope 1": [9999.0, 9999.0],
            "Scope 2": [0.0, 0.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, {"Scope 1"}
        )

        expected_summary = (
            f"`SUMMARY [scope]`\n"
            f"{under_test.SummaryMsg.ZERO_QUOTA_EXCEEDED.value}\n"
            f"\t- Scope 1"
        )

        self.assertEqual(summary, expected_summary)

    def test_zero_quota_violated_mixed(self):
        scope_percentages = {
            "Scope 1": [101.0, 0.0],
            "Scope 2": [9999.0, 9999.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, {"Scope 2"}
        )

        expected_summary = (
            f"`SUMMARY [scope]` "
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED.value}\n"
            f"\t- Scope 1\n"
            f"{under_test.SummaryMsg.ZERO_QUOTA_EXCEEDED.value}\n"
            f"\t- Scope 2"
        )

        self.assertEqual(summary, expected_summary)

    def test_zero_quota_label_green(self):
        # A label tracked as zero-quota but with no issues should still show as GREEN
        scope_percentages = {
            "Scope 1": [0.0, 0.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(
            "scope", scope_percentages, {"Scope 1"}
        )

        expected_summary = f"`SUMMARY [scope]` " f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}"

        self.assertEqual(summary, expected_summary)
