"""Unit tests for bazel.wrapper_hook.query_failure_diagnostic."""

import io
import subprocess
import unittest
from unittest import mock

from bazel.wrapper_hook import query_failure_diagnostic


class ChunkMatcherTest(unittest.TestCase):
    def test_matches_a_marker_split_across_chunks(self):
        matcher = query_failure_diagnostic._ChunkMatcher((b"no such package 'conditions'",))

        matcher.feed(b"ERROR: no such package 'cond")
        self.assertFalse(matcher.found)
        matcher.feed(b"itions'")

        self.assertTrue(matcher.found)


class FormatCqueryCommandTest(unittest.TestCase):
    def test_preserves_startup_flags_query_expression_and_extra_flags(self):
        command = [
            "/path/to/bazel",
            "--output_base=/tmp/query output",
            "query",
            "rdeps(//..., //src/mongo:target)",
            "--keep_going",
        ]

        self.assertEqual(
            query_failure_diagnostic._format_cquery_command(command),
            "bazel '--output_base=/tmp/query output' cquery "
            "'rdeps(//..., //src/mongo:target)' --keep_going",
        )

    def test_returns_none_for_non_query_command(self):
        self.assertIsNone(query_failure_diagnostic._format_cquery_command(["bazel", "build"]))

    def test_diagnostic_has_no_leading_blank_line(self):
        self.assertTrue(query_failure_diagnostic._DIAGNOSTIC.startswith("WARNING:"))


class RunBazelTest(unittest.TestCase):
    def test_uses_a_pty_for_interactive_stderr(self):
        stderr = mock.Mock()
        stderr.isatty.return_value = True

        with mock.patch.object(
            query_failure_diagnostic, "_run_with_pty", return_value=7
        ) as run_with_pty:
            returncode = query_failure_diagnostic.run_bazel(
                ["bazel", "query", "//src/mongo:target"], stderr
            )

        self.assertEqual(returncode, 7)
        run_with_pty.assert_called_once_with(["bazel", "query", "//src/mongo:target"], stderr)

    def test_reports_an_unlaunchable_bazel_like_a_shell_command(self):
        stderr = io.StringIO()
        error = FileNotFoundError(2, "No such file or directory", "/missing/bazel")

        with mock.patch.object(query_failure_diagnostic.subprocess, "Popen", side_effect=error):
            returncode = query_failure_diagnostic.run_bazel(["/missing/bazel", "query"], stderr)

        self.assertEqual(returncode, 127)
        self.assertIn("/missing/bazel: [Errno 2] No such file or directory", stderr.getvalue())

    def test_forwards_output_and_prints_hint_for_target_failure(self):
        bazel_stderr = (
            b"ERROR: Evaluation of query failed: preloading transitive closure failed: "
            b"no such package 'conditions'\n"
        )
        process = mock.Mock()
        process.stderr = io.BytesIO(bazel_stderr)
        process.wait.return_value = 7
        stderr = io.StringIO()
        command = ["/path/to/bazel", "query", "rdeps(//..., //src/mongo:target)"]

        with mock.patch.object(
            query_failure_diagnostic.subprocess, "Popen", return_value=process
        ) as popen:
            returncode = query_failure_diagnostic.run_bazel(command, stderr)

        self.assertEqual(returncode, 7)
        self.assertEqual(
            popen.call_args,
            mock.call(command, stdout=None, stderr=subprocess.PIPE),
        )
        output = stderr.getvalue()
        self.assertTrue(output.startswith(bazel_stderr.decode()))
        self.assertIn("WARNING: Known Bazel query limitation detected.", output)
        self.assertIn(
            "bazel cquery 'rdeps(//..., //src/mongo:target)'",
            output,
        )

    def test_does_not_print_hint_for_success_or_other_errors(self):
        for returncode, bazel_stderr in (
            (0, b"no such package 'conditions'\n"),
            (1, b"ERROR: Evaluation of query failed: no such package 'other'\n"),
        ):
            process = mock.Mock()
            process.stderr = io.BytesIO(bazel_stderr)
            process.wait.return_value = returncode
            stderr = io.StringIO()

            with mock.patch.object(
                query_failure_diagnostic.subprocess, "Popen", return_value=process
            ):
                self.assertEqual(
                    query_failure_diagnostic.run_bazel(
                        ["bazel", "query", "//src/mongo:target"], stderr
                    ),
                    returncode,
                )

            self.assertEqual(stderr.getvalue(), bazel_stderr.decode())


if __name__ == "__main__":
    unittest.main()
