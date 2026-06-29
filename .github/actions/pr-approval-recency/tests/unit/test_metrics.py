"""Unit tests for the pure metrics helpers (stdlib only)."""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2]))
from validate_pr_approval_recency import (
    _build_review_json,
    _logfmt_line,
    _logfmt_value,
    _post_approval_diff_stats,
    write_logfmt,
)


class TestLogfmtValue(unittest.TestCase):
    def test_plain_string_unquoted(self):
        self.assertEqual(_logfmt_value("APPROVED"), "APPROVED")

    def test_integer_unquoted(self):
        self.assertEqual(_logfmt_value(42), "42")

    def test_bool_lowercased(self):
        self.assertEqual(_logfmt_value(True), "true")
        self.assertEqual(_logfmt_value(False), "false")

    def test_value_with_space_is_quoted(self):
        self.assertEqual(_logfmt_value("fix the bug"), '"fix the bug"')

    def test_empty_string_is_quoted(self):
        self.assertEqual(_logfmt_value(""), '""')

    def test_value_with_equals_is_quoted(self):
        self.assertEqual(_logfmt_value("a=b"), '"a=b"')

    def test_embedded_quote_is_escaped(self):
        self.assertEqual(_logfmt_value('say "hi"'), '"say \\"hi\\""')

    def test_newline_replaced_with_space(self):
        self.assertEqual(_logfmt_value("line1\nline2"), '"line1 line2"')

    def test_backslash_is_escaped(self):
        # A bare backslash in a quoted value must be doubled
        self.assertEqual(_logfmt_value("a\\b c"), '"a\\\\b c"')

    def test_backslash_before_quote_keeps_escaping_order(self):
        # backslash + quote: \\ must be emitted before \", not the other way around
        self.assertEqual(_logfmt_value('a\\"'), '"a\\\\\\""')


class TestLogfmtLine(unittest.TestCase):
    def test_line_preserves_insertion_order(self):
        line = _logfmt_line({"result": "pass", "reason": "sha_match", "pr_number": 42})
        self.assertEqual(line, "result=pass reason=sha_match pr_number=42")

    def test_line_quotes_values_with_spaces(self):
        line = _logfmt_line({"pr_title": "fix the bug", "result": "pass"})
        self.assertEqual(line, 'pr_title="fix the bug" result=pass')


class TestWriteLogfmt(unittest.TestCase):
    def test_appends_a_single_line(self):
        import tempfile, os

        tmp = tempfile.mkdtemp()
        path = os.path.join(tmp, "buildevents.txt")
        write_logfmt(path, {"result": "pass", "reason": "sha_match"})
        write_logfmt(path, {"result": "fail", "reason": "content_changed"})
        lines = Path(path).read_text().splitlines()
        self.assertEqual(
            lines,
            [
                "result=pass reason=sha_match",
                "result=fail reason=content_changed",
            ],
        )


class TestPostApprovalDiffStats(unittest.TestCase):
    def test_no_difference_is_zero(self):
        content = {"a.cpp": ["+x", "-y"]}
        self.assertEqual(_post_approval_diff_stats(content, content), (0, 0))

    def test_modified_file_counts_symmetric_difference(self):
        approved = {"a.cpp": ["+x", "-y"]}
        current = {"a.cpp": ["+x", "-z"]}  # -y removed, -z added -> 2 differing lines
        self.assertEqual(_post_approval_diff_stats(approved, current), (1, 2))

    def test_added_file_counts_all_its_lines(self):
        approved = {}
        current = {"new.cpp": ["+a", "+b", "+c"]}
        self.assertEqual(_post_approval_diff_stats(approved, current), (1, 3))

    def test_removed_file_counts_all_its_lines(self):
        approved = {"gone.cpp": ["+a", "+b"]}
        current = {}
        self.assertEqual(_post_approval_diff_stats(approved, current), (1, 2))

    def test_binary_change_counts_file_but_zero_loc(self):
        approved = {"img.png": "blob:111"}
        current = {"img.png": "blob:222"}
        self.assertEqual(_post_approval_diff_stats(approved, current), (1, 0))


class TestReasonMapping(unittest.TestCase):
    def test_pass_reasons_map_to_exit_zero(self):
        from validate_pr_approval_recency import (
            REASON_CONTENT_UNCHANGED,
            REASON_PENDING_REVIEW,
            REASON_SHA_MATCH,
            RESULT_PASS,
            REASON_TO_RESULT,
            _status_for_reason,
            STATUS_OK,
        )

        for reason in (REASON_PENDING_REVIEW, REASON_SHA_MATCH, REASON_CONTENT_UNCHANGED):
            self.assertEqual(REASON_TO_RESULT[reason], RESULT_PASS)
            self.assertEqual(_status_for_reason(reason), STATUS_OK)

    def test_fail_reasons_map_to_exit_one(self):
        from validate_pr_approval_recency import (
            REASON_CONTENT_CHANGED,
            REASON_FILE_LIMIT,
            REASON_NO_APPROVALS,
            RESULT_FAIL,
            REASON_TO_RESULT,
            _status_for_reason,
            STATUS_ERROR,
        )

        for reason in (REASON_CONTENT_CHANGED, REASON_NO_APPROVALS, REASON_FILE_LIMIT):
            self.assertEqual(REASON_TO_RESULT[reason], RESULT_FAIL)
            self.assertEqual(_status_for_reason(reason), STATUS_ERROR)

    def test_api_error_maps_to_error_and_exit_one(self):
        from validate_pr_approval_recency import (
            REASON_API_ERROR,
            RESULT_ERROR,
            REASON_TO_RESULT,
            _status_for_reason,
            STATUS_ERROR,
        )

        self.assertEqual(REASON_TO_RESULT[REASON_API_ERROR], RESULT_ERROR)
        self.assertEqual(_status_for_reason(REASON_API_ERROR), STATUS_ERROR)


class TestBuildReviewJson(unittest.TestCase):
    def test_needs_review_for_content_changed(self):
        metrics = {
            "org": "10gen",
            "repo": "mongo",
            "pr_number": 42,
            "reason": "content_changed",
            "result": "fail",
            "head_sha": "abc",
            "approval_sha": "def",
            "post_approval_changed_file_paths": ["src/a.cpp", "src/b.cpp"],
        }
        blob = _build_review_json(metrics)
        parsed = json.loads(blob)
        self.assertIs(parsed["needs_review"], True)
        self.assertEqual(parsed["changed_files"], ["src/a.cpp", "src/b.cpp"])
        self.assertEqual(parsed["pr_number"], 42)
        self.assertNotIn("merged_at", parsed)

    def test_no_review_for_sha_match(self):
        metrics = {
            "org": "10gen",
            "repo": "mongo",
            "pr_number": 7,
            "reason": "sha_match",
            "result": "pass",
            "head_sha": "x",
            "approval_sha": "x",
        }
        parsed = json.loads(_build_review_json(metrics))
        self.assertIs(parsed["needs_review"], False)
        self.assertEqual(parsed["changed_files"], [])


if __name__ == "__main__":
    unittest.main()
