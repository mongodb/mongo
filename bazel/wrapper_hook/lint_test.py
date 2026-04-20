"""Unit tests for bazel.wrapper_hook.lint."""

import contextlib
import io
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from bazel.wrapper_hook import lint


class RefreshModuleLockfileTest(unittest.TestCase):
    def test_check_mode_fails_when_refresh_changes_lockfile(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            lockfile = pathlib.Path(temp_dir) / "MODULE.bazel.lock"
            lockfile.write_text("before\n", encoding="utf-8")

            def fake_run(args, **kwargs):
                self.assertEqual(args, ["bazel", "mod", "deps", "--lockfile_mode=refresh"])
                lockfile.write_text("after\n", encoding="utf-8")
                return subprocess.CompletedProcess(args, 0)

            runner = lint.LintRunner(keep_going=False, bazel_bin="bazel")
            with mock.patch.object(lint.subprocess, "run", side_effect=fake_run):
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout):
                    with self.assertRaises(lint.LinterFail):
                        runner.refresh_module_lockfile(
                            fix=False,
                            dry_run=False,
                            lockfile_path=lockfile,
                        )

            self.assertTrue(runner.fail)
            self.assertEqual(lockfile.read_text(encoding="utf-8"), "after\n")
            self.assertIn("--- a/tmp/", stdout.getvalue())
            self.assertIn("+++ b/tmp/", stdout.getvalue())
            self.assertIn("-before\n+after\n", stdout.getvalue())

    def test_check_mode_records_lockfile_diff_for_wrapper_footer(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            lockfile = pathlib.Path(temp_dir) / "MODULE.bazel.lock"
            lockfile.write_text("before\n", encoding="utf-8")
            detail_path = pathlib.Path(temp_dir) / "lint_failure_detail.txt"
            detail_path.write_text("", encoding="utf-8")

            def fake_run(args, **kwargs):
                self.assertEqual(args, ["bazel", "mod", "deps", "--lockfile_mode=refresh"])
                lockfile.write_text("after\n", encoding="utf-8")
                return subprocess.CompletedProcess(args, 0)

            runner = lint.LintRunner(keep_going=False, bazel_bin="bazel")
            with (
                mock.patch.dict(
                    os.environ,
                    {lint.LINT_FAILURE_DETAIL_ENV_VAR: str(detail_path)},
                    clear=False,
                ),
                mock.patch.object(lint.subprocess, "run", side_effect=fake_run),
            ):
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout):
                    with self.assertRaises(lint.LinterFail):
                        runner.refresh_module_lockfile(
                            fix=False,
                            dry_run=False,
                            lockfile_path=lockfile,
                        )

            self.assertTrue(runner.fail)
            self.assertEqual(
                detail_path.read_text(encoding="utf-8"),
                f"{lockfile} has diffs after refresh\n\n"
                f"--- a/{str(lockfile).lstrip('/\\\\')}\n"
                f"+++ b/{str(lockfile).lstrip('/\\\\')}\n"
                "@@ -1 +1 @@\n"
                "-before\n"
                "+after",
            )
            self.assertNotIn("--- a/tmp/", stdout.getvalue())

    def test_check_mode_passes_when_refresh_keeps_patch_applied_lockfile(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            lockfile = pathlib.Path(temp_dir) / "MODULE.bazel.lock"
            lockfile.write_text("after\n", encoding="utf-8")

            def fake_run(args, **kwargs):
                self.assertEqual(args, ["bazel", "mod", "deps", "--lockfile_mode=refresh"])
                return subprocess.CompletedProcess(args, 0)

            runner = lint.LintRunner(keep_going=False, bazel_bin="bazel")
            stdout = io.StringIO()
            with (
                mock.patch.object(lint.subprocess, "run", side_effect=fake_run),
                contextlib.redirect_stdout(stdout),
            ):
                runner.refresh_module_lockfile(
                    fix=False,
                    dry_run=False,
                    lockfile_path=lockfile,
                )

            self.assertFalse(runner.fail)
            self.assertEqual(lockfile.read_text(encoding="utf-8"), "after\n")
            self.assertIn(f"{lockfile} is up to date.", stdout.getvalue())

    def test_fix_mode_updates_lockfile_and_passes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            lockfile = pathlib.Path(temp_dir) / "MODULE.bazel.lock"
            lockfile.write_text("before\n", encoding="utf-8")

            def fake_run(args, **kwargs):
                self.assertEqual(args, ["bazel", "mod", "deps", "--lockfile_mode=refresh"])
                lockfile.write_text("after\n", encoding="utf-8")
                return subprocess.CompletedProcess(args, 0)

            runner = lint.LintRunner(keep_going=False, bazel_bin="bazel")
            with mock.patch.object(lint.subprocess, "run", side_effect=fake_run):
                with contextlib.redirect_stdout(io.StringIO()):
                    runner.refresh_module_lockfile(
                        fix=True,
                        dry_run=False,
                        lockfile_path=lockfile,
                    )

            self.assertFalse(runner.fail)
            self.assertEqual(lockfile.read_text(encoding="utf-8"), "after\n")

    def test_fix_dry_run_restores_lockfile_and_passes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            lockfile = pathlib.Path(temp_dir) / "MODULE.bazel.lock"
            lockfile.write_text("before\n", encoding="utf-8")

            def fake_run(args, **kwargs):
                self.assertEqual(args, ["bazel", "mod", "deps", "--lockfile_mode=refresh"])
                lockfile.write_text("after\n", encoding="utf-8")
                return subprocess.CompletedProcess(args, 0)

            runner = lint.LintRunner(keep_going=False, bazel_bin="bazel")
            with mock.patch.object(lint.subprocess, "run", side_effect=fake_run):
                with contextlib.redirect_stdout(io.StringIO()):
                    runner.refresh_module_lockfile(
                        fix=True,
                        dry_run=True,
                        lockfile_path=lockfile,
                    )

            self.assertFalse(runner.fail)
            self.assertEqual(lockfile.read_text(encoding="utf-8"), "before\n")


if __name__ == "__main__":
    unittest.main()
