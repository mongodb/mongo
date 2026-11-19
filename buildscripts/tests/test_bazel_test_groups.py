"""Tests for partitioning the tests selected by a Bazel build."""

import json
import tempfile
import unittest
from pathlib import Path

from buildscripts.bazel_test_groups import partition_tests


class BazelTestGroupsTest(unittest.TestCase):
    def partition(self, events: list[dict]) -> tuple[list[str], list[str]]:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "build_events.json"
            path.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
            return partition_tests(path)

    @staticmethod
    def configured(label: str, tags: list[str]) -> dict:
        return {
            "id": {"targetConfigured": {"label": label}},
            "configured": {"testSize": "MEDIUM", "tag": tags},
        }

    @staticmethod
    def completed(label: str) -> dict:
        return {"id": {"targetCompleted": {"label": label}}, "completed": {"success": True}}

    def test_partitions_selected_tests_without_overlap(self):
        regular = "//tests:regular_test"
        sensitive = "//tests:sensitive_test"
        events = [
            self.configured(sensitive, ["mongo_unittest", "windows_timing_sensitive"]),
            self.configured(regular, ["mongo_unittest"]),
            self.completed(regular),
            self.completed(sensitive),
        ]
        self.assertEqual(([regular], [sensitive]), self.partition(events))

    def test_excludes_libraries_and_skipped_or_failed_tests(self):
        events = [
            {"id": {"targetConfigured": {"label": "//tests:library"}}, "configured": {}},
            self.completed("//tests:library"),
            self.configured("//tests:skipped_test", ["windows_timing_sensitive"]),
            {"id": {"targetCompleted": {"label": "//tests:skipped_test"}}, "aborted": {}},
            self.configured("//tests:failed_test", []),
            {"id": {"targetCompleted": {"label": "//tests:failed_test"}}, "completed": {}},
        ]
        self.assertEqual(([], []), self.partition(events))

    def test_handles_event_order_and_duplicate_labels(self):
        events = [
            self.completed("//tests:z_test"),
            self.configured("//tests:z_test", []),
            self.configured("//tests:z_test", []),
            self.configured("//tests:a_test", []),
            self.completed("//tests:a_test"),
        ]
        self.assertEqual((["//tests:a_test", "//tests:z_test"], []), self.partition(events))

    def test_missing_tags_are_regular_tests(self):
        self.assertEqual(
            (["//tests:regular_test"], []),
            self.partition(
                [
                    {
                        "id": {"targetConfigured": {"label": "//tests:regular_test"}},
                        "configured": {"testSize": "SMALL"},
                    },
                    self.completed("//tests:regular_test"),
                ]
            ),
        )

    def test_empty_build_has_no_tests(self):
        self.assertEqual(([], []), self.partition([]))

    def test_invalid_build_events_fail_instead_of_silently_skipping_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "build_events.json"
            path.write_text("not json\n", encoding="utf-8")
            with self.assertRaises(json.JSONDecodeError):
                partition_tests(path)


if __name__ == "__main__":
    unittest.main()
