"""Tests for strict patch bypass parsing and candidate-bounded selection."""

import json
import unittest

from buildscripts.resmokelib.utils import test_selection as under_test


class TestTestSelection(unittest.TestCase):
    def test_merge_preserves_candidates_and_order(self):
        self.assertEqual(
            (["b.js", "a.js"], ["c.js"]),
            under_test.merge_selected_tests(
                ["b.js", "a.js", "c.js"], ["a.js", "outside.js"], {"b.js", "outside.js"}
            ),
        )

    def test_parser_rejects_invalid_input(self):
        for raw in (
            "broken",
            "null",
            "{}",
            '"a.js"',
            "1",
            "true",
            "[1]",
            "[null]",
            '[""]',
            " ",
            '["a.js", false]',
            None,
            [],
        ):
            with self.subTest(raw=raw):
                with self.assertRaisesRegex(ValueError, "test_selection_bypass_quarantine"):
                    under_test.parse_bypass_tests(raw)

    def test_parser_preserves_exact_names(self):
        names = ["a,b.js", "a b.js", 'a"b.js', r"a\b.js", "a=b.js", "$(name).js", " ", " a.js "]
        self.assertEqual(set(names), under_test.parse_bypass_tests(json.dumps(names)))
        self.assertEqual(
            (names, ["a.js", "a/b.js"]),
            under_test.merge_selected_tests(
                names + ["a.js", "a/b.js"], [], under_test.parse_bypass_tests(json.dumps(names))
            ),
        )

    def test_missing_empty_and_duplicate_inputs(self):
        self.assertEqual(set(), under_test.parse_bypass_tests(""))
        self.assertEqual(set(), under_test.parse_bypass_tests(" [] "))
        self.assertEqual({"a.js"}, under_test.parse_bypass_tests('["a.js","a.js"]'))
        self.assertEqual(([], []), under_test.merge_selected_tests([], ["a.js"], {"a.js"}))

    def test_candidate_duplicates_are_preserved(self):
        self.assertEqual(
            (["a.js", "a.js"], ["b.js", "b.js"]),
            under_test.merge_selected_tests(["a.js", "b.js", "a.js", "b.js"], [], {"a.js"}),
        )

    def test_empty_bypass_preserves_selection(self):
        self.assertEqual(
            (["a.js"], ["b.js"]),
            under_test.merge_selected_tests(["b.js", "a.js"], ["a.js", "outside.js"], set()),
        )
