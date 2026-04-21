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


class RunRulesLintTest(unittest.TestCase):
    """Tests for the two-pass fix/check behavior in run_rules_lint."""

    def setUp(self):
        # Mock everything in the heavy preamble so tests only exercise the
        # rules_lint aspects / fix-pass / check-pass logic.
        self._patches = [
            mock.patch.object(lint.platform, "system", return_value="Linux"),
            mock.patch.object(lint, "create_build_files_in_new_js_dirs"),
            mock.patch.object(lint, "list_files_with_targets", return_value=[]),
            mock.patch.object(lint.LintRunner, "refresh_module_lockfile"),
            mock.patch.object(lint.LintRunner, "list_files_without_targets"),
            mock.patch.object(lint.LintRunner, "run_bazel"),
            mock.patch.object(lint, "_git_distance", return_value=0),
            mock.patch.object(lint, "_get_files_changed_since_fork_point", return_value=[]),
        ]
        for p in self._patches:
            p.start()

    def tearDown(self):
        for p in self._patches:
            p.stop()

    def _run(
        self,
        extra_args: list[str],
        *,
        check_report: str | None = None,
        fix_patch: str | None = None,
    ) -> tuple[list[list[str]], bool, lint.LinterFail | None]:
        """
        Invoke run_rules_lint with the preamble mocked out.

        check_report: content written into the .out file that the check pass "finds".
                      None means no violations (empty report).
        fix_patch:    content written into the .patch file that the fix pass "finds".
                      None means nothing to fix.

        Returns (bazel_build_calls, patch_was_applied, raised_exception).
        """
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = pathlib.Path(tmpdir)
            check_report_path = str(tmpdir_path / "check.out")
            fix_patch_path = str(tmpdir_path / "fix.patch")
            check_events_path = str(tmpdir_path / "check_events")
            fix_events_path = str(tmpdir_path / "fix_events")

            if check_report is not None:
                pathlib.Path(check_report_path).write_text(check_report, encoding="utf-8")
            if fix_patch is not None:
                pathlib.Path(fix_patch_path).write_text(fix_patch, encoding="utf-8")

            bazel_build_calls: list[list[str]] = []
            patch_applied = [False]

            def fake_run(args, **kwargs):
                args = list(args)
                if args[:2] == ["bazel", "build"]:
                    bazel_build_calls.append(args)
                    return subprocess.CompletedProcess(args, 0)
                if args[0] == "jq":
                    # Array should look something like this:
                    #   ["jq", "--arg", "ext", ".out"/".patch", "--raw-output", expr, events_path]
                    ext = args[3]
                    events_path = args[-1]
                    if ext == ".patch" and events_path == fix_events_path and fix_patch is not None:
                        stdout = fix_patch_path
                    elif (
                        ext == ".out"
                        and events_path == check_events_path
                        and check_report is not None
                    ):
                        stdout = check_report_path
                    else:
                        stdout = ""
                    return subprocess.CompletedProcess(args, 0, stdout=stdout)
                if args[0] == "patch":
                    patch_applied[0] = True
                    if "stdin" in kwargs:
                        kwargs["stdin"].close()
                    return subprocess.CompletedProcess(args, 0)
                return subprocess.CompletedProcess(args, 0)

            # mkstemp is called twice in fix mode (check events first,
            # fix events second) and once in check-only mode.
            counter = [0]
            events_paths = [check_events_path, fix_events_path]

            def fake_mkstemp():
                path = events_paths[counter[0]]
                counter[0] += 1
                return (0, path)

            raised: lint.LinterFail | None = None
            with (
                mock.patch.object(lint.subprocess, "run", side_effect=fake_run),
                mock.patch.object(lint.tempfile, "mkstemp", side_effect=fake_mkstemp),
                mock.patch.object(lint.os, "close"),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                try:
                    lint.run_rules_lint("bazel", ["--origin-branch=origin/master"] + extra_args)
                except lint.LinterFail as e:
                    raised = e

            return bazel_build_calls, patch_applied[0], raised

    def test_check_only_no_violations_runs_single_build_and_passes(self):
        builds, patched, exc = self._run([])
        self.assertIsNone(exc)
        self.assertEqual(len(builds), 1)
        self.assertNotIn("--@aspect_rules_lint//lint:fix", builds[0])
        self.assertFalse(patched)

    def test_check_only_with_violations_runs_single_build_and_fails(self):
        builds, patched, exc = self._run(
            [], check_report="F841 local variable `result` is assigned to but never used"
        )
        self.assertIsInstance(exc, lint.LinterFail)
        self.assertEqual(len(builds), 1)
        self.assertFalse(patched)

    def test_fix_with_only_fixable_violations_applies_patch_and_passes(self):
        patch_content = "--- a/foo.py\n+++ b/foo.py\n@@ -1 +1 @@\n-import os,sys\n+import os\n"
        builds, patched, exc = self._run(["--fix"], fix_patch=patch_content)
        self.assertIsNone(exc)
        self.assertEqual(len(builds), 2)
        # First build is the fix pass — must carry the fix flags.
        self.assertIn("--@aspect_rules_lint//lint:fix", builds[0])
        self.assertIn("--output_groups=rules_lint_patch", builds[0])
        # Second build is the check pass — must not carry fix flags.
        self.assertNotIn("--@aspect_rules_lint//lint:fix", builds[1])
        self.assertTrue(patched)

    def test_fix_with_unfixable_violations_remaining_applies_patch_and_fails(self):
        patch_content = "--- a/foo.py\n+++ b/foo.py\n@@ -1 +1 @@\n-import os,sys\n+import os\n"
        builds, patched, exc = self._run(
            ["--fix"],
            fix_patch=patch_content,
            check_report="F841 local variable `result` is assigned to but never used",
        )
        self.assertIsInstance(exc, lint.LinterFail)
        self.assertEqual(len(builds), 2)
        self.assertIn("--@aspect_rules_lint//lint:fix", builds[0])
        self.assertNotIn("--@aspect_rules_lint//lint:fix", builds[1])
        self.assertTrue(patched)

    def test_fix_with_only_unfixable_violations_runs_two_builds_and_fails(self):
        builds, patched, exc = self._run(
            ["--fix"], check_report="F841 local variable `result` is assigned to but never used"
        )
        self.assertIsInstance(exc, lint.LinterFail)
        self.assertEqual(len(builds), 2)
        self.assertFalse(patched)

    def test_dry_run_prints_patches_without_applying_them(self):
        patch_content = "--- a/foo.py\n+++ b/foo.py\n@@ -1 +1 @@\n-import os,sys\n+import os\n"
        builds, patched, exc = self._run(["--fix", "--dry-run"], fix_patch=patch_content)
        self.assertIsNone(exc)
        self.assertEqual(len(builds), 2)
        self.assertFalse(patched)  # patch -p1 must NOT be called in dry-run mode


if __name__ == "__main__":
    unittest.main()
