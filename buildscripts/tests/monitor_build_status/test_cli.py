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

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(scope_percentages)

        expected_summary = (
            f"{under_test.SummaryMsg.PREFIX.value} "
            f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(summary, expected_summary)

    def test_all_thresholds_are_100(self):
        scope_percentages = {
            "Scope 1": [100.0, 100.0, 100.0],
            "Scope 2": [100.0, 100.0, 100.0],
            "Scope 3": [100.0, 100.0, 100.0],
            "Scope 4": [100.0, 100.0, 100.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(scope_percentages)

        expected_summary = (
            f"{under_test.SummaryMsg.PREFIX.value} "
            f"{under_test.SummaryMsg.BELOW_THRESHOLDS.value}\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(summary, expected_summary)

    def test_some_threshold_exceeded_100(self):
        scope_percentages = {
            "Scope 1": [101.0, 0.0, 0.0],
            "Scope 2": [0.0, 101.0, 0.0],
            "Scope 3": [0.0, 0.0, 101.0],
            "Scope 4": [0.0, 0.0, 0.0],
        }

        summary = under_test.MonitorBuildStatusOrchestrator._summarize(scope_percentages)

        expected_summary = (
            f"{under_test.SummaryMsg.PREFIX.value} "
            f"{under_test.SummaryMsg.THRESHOLD_EXCEEDED.value}\n"
            f"\t- Scope 1\n\t- Scope 2\n\t- Scope 3\n\n"
            f"{under_test.SummaryMsg.PLAYBOOK_REFERENCE.value}\n"
        )

        self.assertEqual(summary, expected_summary)
