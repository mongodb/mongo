"""Unit tests for sort_backport_multiversion.py."""

import unittest

import yaml

from buildscripts.sort_backport_multiversion import (
    _extract_header,
    _IndentedDumper,
    _sort_all_entries,
)


class TestExtractHeader(unittest.TestCase):
    def test_no_header(self):
        self.assertEqual(_extract_header("key: value\n"), "")

    def test_single_comment_line(self):
        text = "# comment\nkey: value\n"
        self.assertEqual(_extract_header(text), "# comment\n")

    def test_multiple_comment_lines(self):
        text = "# line 1\n# line 2\nkey: value\n"
        self.assertEqual(_extract_header(text), "# line 1\n# line 2\n")

    def test_stops_at_blank_line(self):
        text = "# comment\n\nkey: value\n"
        self.assertEqual(_extract_header(text), "# comment\n")

    def test_only_comments(self):
        text = "# a\n# b\n"
        self.assertEqual(_extract_header(text), "# a\n# b\n")

    def test_empty_string(self):
        self.assertEqual(_extract_header(""), "")


class TestSortAllEntries(unittest.TestCase):
    def test_sort_last_continuous(self):
        cases = [
            {
                "name": "sorts by test_file",
                "input_entries": [
                    {"test_file": "b_test.js", "ticket": "SERVER-1"},
                    {"test_file": "a_test.js", "ticket": "SERVER-2"},
                ],
                "expected_result": [
                    {"test_file": "a_test.js", "ticket": "SERVER-2"},
                    {"test_file": "b_test.js", "ticket": "SERVER-1"},
                ],
            },
            {
                "name": "sorts by ticket as tiebreaker",
                "input_entries": [
                    {"test_file": "same.js", "ticket": "SERVER-2"},
                    {"test_file": "same.js", "ticket": "SERVER-1"},
                ],
                "expected_result": [
                    {"test_file": "same.js", "ticket": "SERVER-1"},
                    {"test_file": "same.js", "ticket": "SERVER-2"},
                ],
            },
            {
                "name": "empty entries list",
                "input_entries": [],
                "expected_result": [],
            },
            {
                "name": "single entry unchanged",
                "input_entries": [{"test_file": "only.js", "ticket": "SERVER-99"}],
                "expected_result": [{"test_file": "only.js", "ticket": "SERVER-99"}],
            },
            {
                "name": "already sorted unchanged",
                "input_entries": [
                    {"test_file": "a.js", "ticket": "SERVER-1"},
                    {"test_file": "b.js", "ticket": "SERVER-2"},
                ],
                "expected_result": [
                    {"test_file": "a.js", "ticket": "SERVER-1"},
                    {"test_file": "b.js", "ticket": "SERVER-2"},
                ],
            },
        ]
        for case in cases:
            with self.subTest(case["name"]):
                input_entries = case["input_entries"]
                expected_result = case["expected_result"]
                data = {"last-continuous": {"all": input_entries}}
                _sort_all_entries(data)
                self.assertEqual(data["last-continuous"]["all"], expected_result)

    def test_sorts_both_sections_independently(self):
        input_lc = [{"test_file": "z.js", "ticket": "T-1"}, {"test_file": "a.js", "ticket": "T-2"}]
        input_llts = [
            {"test_file": "y.js", "ticket": "T-3"},
            {"test_file": "b.js", "ticket": "T-4"},
        ]
        expected_result = {
            "last-continuous": {
                "all": [
                    {"test_file": "a.js", "ticket": "T-2"},
                    {"test_file": "z.js", "ticket": "T-1"},
                ]
            },
            "last-lts": {
                "all": [
                    {"test_file": "b.js", "ticket": "T-4"},
                    {"test_file": "y.js", "ticket": "T-3"},
                ]
            },
        }
        data = {"last-continuous": {"all": input_lc}, "last-lts": {"all": input_llts}}
        _sort_all_entries(data)
        self.assertEqual(data, expected_result)

    def test_missing_section_is_skipped(self):
        data = {}
        _sort_all_entries(data)  # should not raise

    def test_null_section_is_skipped(self):
        data = {"last-continuous": None, "last-lts": None}
        _sort_all_entries(data)  # should not raise

    def test_missing_all_key_is_skipped(self):
        data = {"last-continuous": {"other_key": []}}
        _sort_all_entries(data)  # should not raise

    def test_sorts_in_place(self):
        input_entries = [
            {"test_file": "b.js", "ticket": "T-1"},
            {"test_file": "a.js", "ticket": "T-2"},
        ]
        original_list = input_entries
        data = {"last-continuous": {"all": input_entries}}
        _sort_all_entries(data)
        self.assertIs(data["last-continuous"]["all"], original_list)


class TestIndentedDumper(unittest.TestCase):
    def test_sequence_items_are_indented(self):
        data = {"key": [{"a": 1}, {"b": 2}]}
        result = yaml.dump(data, Dumper=_IndentedDumper, default_flow_style=False, indent=2)
        # Each list item dash should be indented under its parent key, not flush with it
        for line in result.splitlines():
            if line.lstrip().startswith("-"):
                self.assertTrue(line.startswith("  "), repr(line))

    def test_round_trips_correctly(self):
        data = {
            "last-continuous": {
                "all": [
                    {"test_file": "a.js", "ticket": "SERVER-1"},
                    {"test_file": "b.js", "ticket": "SERVER-2"},
                ]
            }
        }
        dumped = yaml.dump(data, Dumper=_IndentedDumper, default_flow_style=False, indent=2)
        reloaded = yaml.safe_load(dumped)
        self.assertEqual(reloaded, data)


if __name__ == "__main__":
    unittest.main()
