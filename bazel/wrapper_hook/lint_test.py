"""Unit tests for bazel.wrapper_hook.lint."""

import contextlib
import io
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from bazel.wrapper_hook import lint


class RefreshModuleLockfileTest(unittest.TestCase):
    def test_check_mode_fails_when_lockfile_has_git_diff_after_refresh(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            lockfile = pathlib.Path(temp_dir) / "MODULE.bazel.lock"
            lockfile.write_text("before\n", encoding="utf-8")

            def fake_run(args, **kwargs):
                self.assertEqual(args, ["bazel", "mod", "deps", "--lockfile_mode=refresh"])
                lockfile.write_text("after\n", encoding="utf-8")
                return subprocess.CompletedProcess(args, 0)

            runner = lint.LintRunner(keep_going=False, bazel_bin="bazel")
            with (
                mock.patch.object(lint.subprocess, "run", side_effect=fake_run),
                mock.patch.object(lint, "_lockfile_has_git_diff", return_value=True),
                mock.patch.object(lint, "_print_git_diff_against_head"),
            ):
                with contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(lint.LinterFail):
                        runner.refresh_module_lockfile(
                            fix=False,
                            dry_run=False,
                            lockfile_path=lockfile,
                        )

            self.assertTrue(runner.fail)
            self.assertEqual(lockfile.read_text(encoding="utf-8"), "after\n")

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
