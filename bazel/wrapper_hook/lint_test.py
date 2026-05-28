"""Unit tests for bazel.wrapper_hook.lint."""

import contextlib
import importlib
import importlib.abc
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from bazel.wrapper_hook import lint


class BazelCustomFormatterImportTest(unittest.TestCase):
    def test_import_does_not_require_click(self):
        class BlockClickFinder(importlib.abc.MetaPathFinder):
            def find_spec(self, fullname, path, target=None):
                if fullname == "click" or fullname.startswith("click."):
                    raise ModuleNotFoundError("No module named 'click'")
                return None

        module_names = [
            "buildscripts.bazel_custom_formatter",
            "buildscripts.simple_report",
            "click",
        ]
        saved_modules = {name: sys.modules.get(name) for name in module_names}
        for name in module_names:
            sys.modules.pop(name, None)

        finder = BlockClickFinder()
        sys.meta_path.insert(0, finder)
        try:
            module = importlib.import_module("buildscripts.bazel_custom_formatter")
            self.assertTrue(hasattr(module, "validate_tcmalloc_cc_test_coverage"))
            self.assertNotIn("buildscripts.simple_report", sys.modules)
        finally:
            sys.meta_path.remove(finder)
            for name in module_names:
                sys.modules.pop(name, None)
                if saved_modules[name] is not None:
                    sys.modules[name] = saved_modules[name]


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
            mock.patch.object(lint, "list_files_with_targets", return_value=["//:foo.py"]),
            mock.patch.object(lint.LintRunner, "refresh_module_lockfile"),
            mock.patch.object(lint.LintRunner, "list_files_without_targets"),
            mock.patch.object(lint.LintRunner, "run_bazel"),
            mock.patch.object(lint, "_git_distance", return_value=0),
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
        fix_patch: str | list[str] | None = None,
        changed_files: list[str] | None = None,
    ) -> tuple[list[list[str]], int, lint.LinterFail | None]:
        """
        Invoke run_rules_lint with the preamble mocked out.

        check_report: content written into the .out file that the check pass "finds".
                      None means no violations (empty report).
        fix_patch:    content written into the .patch file that the fix pass "finds".
                      None means nothing to fix.

        Returns (bazel_build_calls, patch_apply_count, raised_exception).
        """
        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = pathlib.Path(tmpdir)
            check_report_path = str(tmpdir_path / "check.out")
            check_events_path = str(tmpdir_path / "check_events")
            fix_events_path = str(tmpdir_path / "fix_events")
            fix_patch_paths: list[str] = []
            if changed_files is None:
                changed_files = ["foo.py"]

            if check_report is not None:
                pathlib.Path(check_report_path).write_text(check_report, encoding="utf-8")
            if fix_patch is not None:
                fix_patches = [fix_patch] if isinstance(fix_patch, str) else fix_patch
                for index, fix_patch_contents in enumerate(fix_patches):
                    fix_patch_path = str(tmpdir_path / f"fix_{index}.patch")
                    pathlib.Path(fix_patch_path).write_text(
                        fix_patch_contents,
                        encoding="utf-8",
                    )
                    fix_patch_paths.append(fix_patch_path)

            bazel_build_calls: list[list[str]] = []
            patch_apply_count = [0]

            def fake_run(args, **kwargs):
                args = list(args)
                if args[:2] == ["bazel", "query"]:
                    self.assertEqual(
                        args,
                        [
                            "bazel",
                            "query",
                            'kind(".* rule", same_pkg_direct_rdeps(//:foo.py))',
                            "--output=label",
                        ],
                    )
                    return subprocess.CompletedProcess(args, 0, stdout="//:foo_lib\n")
                if args[:2] == ["bazel", "build"]:
                    bazel_build_calls.append(args)
                    return subprocess.CompletedProcess(args, 0)
                if args[0] == "jq":
                    # Array should look something like this:
                    #   ["jq", "--arg", "ext", ".out"/".patch", "--raw-output", expr, events_path]
                    ext = args[3]
                    events_path = args[-1]
                    if ext == ".patch" and events_path == fix_events_path and fix_patch is not None:
                        stdout = "\n".join(fix_patch_paths)
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
                    patch_apply_count[0] += 1
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
                mock.patch.object(
                    lint,
                    "_get_files_changed_since_fork_point",
                    return_value=changed_files,
                ),
                mock.patch.object(lint.subprocess, "run", side_effect=fake_run),
                mock.patch.object(lint.tempfile, "mkstemp", side_effect=fake_mkstemp),
                mock.patch.object(lint.os, "close"),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                try:
                    lint.run_rules_lint("bazel", ["--origin-branch=origin/master"] + extra_args)
                except lint.LinterFail as e:
                    raised = e

            return bazel_build_calls, patch_apply_count[0], raised

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
        builds, patched, exc = self._run(["--fix", "foo.py"], fix_patch=patch_content)
        self.assertIsNone(exc)
        self.assertEqual(len(builds), 2)
        # First build is the fix pass — must carry the fix flags.
        self.assertIn("--@aspect_rules_lint//lint:fix", builds[0])
        self.assertIn("--output_groups=rules_lint_patch", builds[0])
        # Second build is the check pass — must not carry fix flags.
        self.assertNotIn("--@aspect_rules_lint//lint:fix", builds[1])
        self.assertTrue(patched)
        self.assertNotIn("//...", builds[0])

    def test_fix_with_unfixable_violations_remaining_applies_patch_and_fails(self):
        patch_content = "--- a/foo.py\n+++ b/foo.py\n@@ -1 +1 @@\n-import os,sys\n+import os\n"
        builds, patched, exc = self._run(
            ["--fix", "foo.py"],
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
            ["--fix", "foo.py"],
            check_report="F841 local variable `result` is assigned to but never used",
        )
        self.assertIsInstance(exc, lint.LinterFail)
        self.assertEqual(len(builds), 2)
        self.assertFalse(patched)

    def test_dry_run_prints_patches_without_applying_them(self):
        patch_content = "--- a/foo.py\n+++ b/foo.py\n@@ -1 +1 @@\n-import os,sys\n+import os\n"
        builds, patched, exc = self._run(
            ["--fix", "--dry-run", "foo.py"],
            fix_patch=patch_content,
        )
        self.assertIsNone(exc)
        self.assertEqual(len(builds), 2)
        self.assertFalse(patched)  # patch -p1 must NOT be called in dry-run mode

    def test_fix_skips_duplicate_patch_contents(self):
        patch_content = "--- a/foo.py\n+++ b/foo.py\n@@ -1 +1 @@\n-import os,sys\n+import os\n"
        builds, patched, exc = self._run(
            ["--fix", "foo.py"],
            fix_patch=[patch_content, patch_content],
        )
        self.assertIsNone(exc)
        self.assertEqual(len(builds), 2)
        self.assertEqual(patched, 1)

    def test_no_target_fix_defaults_to_changed_rules_lint_files(self):
        builds, patched, exc = self._run(["--fix"])
        self.assertIsNone(exc)
        self.assertEqual(len(builds), 2)
        self.assertIn("//:foo_lib", builds[0])
        self.assertIn("//:foo_lib", builds[1])
        self.assertNotIn("//:foo.py", builds[0])
        self.assertNotIn("//:foo.py", builds[1])
        self.assertNotIn("//...", builds[0])
        self.assertNotIn("//...", builds[1])
        self.assertEqual(patched, 0)


class ExistingPythonFilesTest(unittest.TestCase):
    def test_filters_deleted_python_paths(self):
        files_to_lint = [
            "buildscripts/sync_repo_with_copybara.py",
            "buildscripts/copybara/sync_repo_with_copybara.py",
            "docs/branching/README.md",
        ]

        with mock.patch.object(
            lint.os.path,
            "exists",
            side_effect=lambda path: path == "buildscripts/copybara/sync_repo_with_copybara.py",
        ):
            self.assertEqual(
                lint._get_existing_python_files(files_to_lint),
                ["buildscripts/copybara/sync_repo_with_copybara.py"],
            )

    def test_maps_main_repo_source_label_to_workspace_path(self):
        self.assertEqual(
            lint._source_label_to_workspace_path("//buildscripts/copybara:generate_evergreen.py"),
            "buildscripts/copybara/generate_evergreen.py",
        )

    def test_maps_local_repository_source_label_to_workspace_path(self):
        self.assertEqual(
            lint._source_label_to_workspace_path("@bazel_rules_mongo//codeowners:parsers/foo.py"),
            "buildscripts/bazel_rules_mongo/codeowners/parsers/foo.py",
        )

    def test_get_rules_lint_source_labels_for_changed_files(self):
        self.assertEqual(
            lint._get_rules_lint_source_labels_for_changed_files(
                [
                    "buildscripts/copybara/generate_evergreen.py",
                    "buildscripts/bazel_rules_mongo/codeowners/parsers/owners_v1.py",
                    "etc/evergreen.yml",
                ],
                [
                    "//buildscripts/copybara:generate_evergreen.py",
                    "@bazel_rules_mongo//codeowners:parsers/owners_v1.py",
                    "//etc:evergreen.yml",
                ],
            ),
            [
                "//buildscripts/copybara:generate_evergreen.py",
                "@bazel_rules_mongo//codeowners:parsers/owners_v1.py",
            ],
        )

    def test_maps_canonical_local_repository_source_label_to_workspace_path(self):
        self.assertEqual(
            lint._source_label_to_workspace_path("@@bazel_rules_mongo//codeowners:parsers/foo.py"),
            "buildscripts/bazel_rules_mongo/codeowners/parsers/foo.py",
        )

    def test_get_rules_lint_targets_for_source_labels_queries_owner_rules(self):
        def fake_run(args, **kwargs):
            self.assertEqual(
                args,
                [
                    "bazel",
                    "query",
                    'kind(".* rule", same_pkg_direct_rdeps(//buildscripts/copybara:generate_evergreen.py))',
                    "--output=label",
                ],
            )
            self.assertTrue(kwargs["capture_output"])
            self.assertTrue(kwargs["text"])
            self.assertFalse(kwargs["check"])
            return subprocess.CompletedProcess(
                args,
                0,
                stdout=(
                    "//buildscripts/copybara:generate_evergreen\n"
                    "//buildscripts/copybara:generate_evergreen_test\n"
                ),
            )

        with mock.patch.object(lint.subprocess, "run", side_effect=fake_run):
            self.assertEqual(
                lint._get_rules_lint_targets_for_source_labels(
                    "bazel",
                    ["//buildscripts/copybara:generate_evergreen.py"],
                ),
                [
                    "//buildscripts/copybara:generate_evergreen",
                    "//buildscripts/copybara:generate_evergreen_test",
                ],
            )

    def test_get_rules_lint_targets_for_changed_files_returns_owner_rules(self):
        with mock.patch.object(
            lint,
            "_get_rules_lint_targets_for_source_labels",
            return_value=["//buildscripts/copybara:generate_evergreen"],
        ) as mock_get_targets:
            self.assertEqual(
                lint._get_rules_lint_targets_for_changed_files(
                    "bazel",
                    ["buildscripts/copybara/generate_evergreen.py"],
                    ["//buildscripts/copybara:generate_evergreen.py"],
                ),
                ["//buildscripts/copybara:generate_evergreen"],
            )

        mock_get_targets.assert_called_once_with(
            "bazel",
            ["//buildscripts/copybara:generate_evergreen.py"],
        )


class CopybaraGeneratedEvergreenCheckTest(unittest.TestCase):
    def test_runs_for_lint_all(self):
        self.assertTrue(lint._should_check_copybara_generated_evergreen(True, []))

    def test_runs_for_copybara_config_change(self):
        self.assertTrue(
            lint._should_check_copybara_generated_evergreen(
                False,
                ["buildscripts/copybara/v8_2.sky"],
            )
        )

    def test_runs_for_generated_copybara_yaml_change(self):
        self.assertTrue(
            lint._should_check_copybara_generated_evergreen(
                False,
                ["etc/evergreen_yml_components/copybara/copybara_gen.yml"],
            )
        )

    def test_skips_unrelated_files(self):
        self.assertFalse(
            lint._should_check_copybara_generated_evergreen(
                False,
                ["src/mongo/db/query/query.cpp"],
            )
        )

    def test_check_mode_runs_generated_yaml_check(self):
        runner = lint.LintRunner(keep_going=False, bazel_bin="bazel")

        with mock.patch.object(runner, "run_bazel", return_value=True) as mock_run_bazel:
            with contextlib.redirect_stdout(io.StringIO()):
                runner.check_copybara_generated_evergreen(fix=False, dry_run=False)

        mock_run_bazel.assert_called_once_with(
            "//buildscripts/copybara:generate_evergreen",
            ["--check"],
        )

    def test_fix_mode_runs_generated_yaml_writer(self):
        runner = lint.LintRunner(keep_going=False, bazel_bin="bazel")

        with mock.patch.object(runner, "run_bazel", return_value=True) as mock_run_bazel:
            with contextlib.redirect_stdout(io.StringIO()):
                runner.check_copybara_generated_evergreen(fix=True, dry_run=False)

        mock_run_bazel.assert_called_once_with("//buildscripts/copybara:generate_evergreen")

    def test_fix_dry_run_keeps_generated_yaml_check_only(self):
        runner = lint.LintRunner(keep_going=False, bazel_bin="bazel")

        with mock.patch.object(runner, "run_bazel", return_value=True) as mock_run_bazel:
            with contextlib.redirect_stdout(io.StringIO()):
                runner.check_copybara_generated_evergreen(fix=True, dry_run=True)

        mock_run_bazel.assert_called_once_with(
            "//buildscripts/copybara:generate_evergreen",
            ["--check"],
        )


if __name__ == "__main__":
    unittest.main()
