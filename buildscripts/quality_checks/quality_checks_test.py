"""Unit tests for the unified repository quality-check runner."""

from __future__ import annotations

import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

from buildscripts.quality_checks import adapters, cli, selection, telemetry
from buildscripts.quality_checks.adapters import CheckContext, RunOutcome
from buildscripts.quality_checks.engine import (
    CheckSelection,
    EngineResult,
    QualityChecksEngine,
    matched_candidates,
    runnable_paths,
    select_checks,
    telemetry_paths,
)
from buildscripts.quality_checks.manifest import CHECKS, RegisteredCheck, validate_manifest
from buildscripts.quality_checks.models import (
    CandidateFile,
    ChangeStatus,
    CheckPhase,
    CheckSpec,
    CheckStatus,
)
from buildscripts.quality_checks.progress import TerminalUI
from buildscripts.quality_checks.selection import DiscoveryResult
from buildscripts.quality_checks.telemetry import MAX_PATH_SAMPLES, TelemetryClient


def _registered(
    check_id: str,
    *,
    group: str = "lint",
    phase: CheckPhase = CheckPhase.SHARED_STATE_SERIAL,
    dependencies: tuple[str, ...] = (),
    supports_fix: bool = False,
    matcher=lambda _candidate: True,
    runner=lambda _context, _files: RunOutcome(0),
) -> RegisteredCheck:
    return RegisteredCheck(
        CheckSpec(
            check_id,
            check_id,
            group,
            phase,
            dependencies=dependencies,
            supports_fix=supports_fix,
        ),
        matcher,
        runner,
    )


class _TelemetryRecorder:
    def __init__(self):
        self.results = []

    def record_result(self, result):
        self.results.append(result)


def _context(tmp: Path, candidates: tuple[CandidateFile, ...]) -> CheckContext:
    return CheckContext(
        repo_root=tmp,
        bazel_real="bazel",
        candidates=candidates,
        all_files=False,
        fix=False,
        origin_branch="origin/master",
        invocation_name="checks",
        explicit_groups=frozenset(),
    )


class ManifestTest(unittest.TestCase):
    def test_shipped_manifest_is_valid_and_ids_are_stable(self):
        validate_manifest()
        ids = {check.spec.check_id for check in CHECKS}
        self.assertIn("format.formatters", ids)
        self.assertIn("bazel.server", ids)
        self.assertIn("lint.target-coverage", ids)
        self.assertIn("lint.module-lockfile", ids)
        self.assertIn("lint.copybara-forbidden-text", ids)
        self.assertIn("lint.rules-lint", ids)
        self.assertIn("codeowners.local", ids)

    def test_duplicate_id_is_rejected(self):
        check = _registered("lint.same")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            validate_manifest((check, check))

    def test_missing_dependency_is_rejected(self):
        check = _registered("lint.child", dependencies=("lint.missing",))
        with self.assertRaisesRegex(ValueError, "unknown dependencies"):
            validate_manifest((check,))

    def test_dependency_cycle_is_rejected(self):
        one = _registered("lint.one", dependencies=("lint.two",))
        two = _registered("lint.two", dependencies=("lint.one",))
        with self.assertRaisesRegex(ValueError, "cycle"):
            validate_manifest((one, two))

    def test_dependency_cannot_point_to_a_later_phase(self):
        later = _registered("lint.later", phase=CheckPhase.SHARED_STATE_SERIAL)
        earlier = _registered(
            "lint.earlier",
            phase=CheckPhase.SAFE_PARALLEL,
            dependencies=("lint.later",),
        )
        with self.assertRaisesRegex(ValueError, "later execution phase"):
            validate_manifest((earlier, later))

    def test_semantic_validators_are_not_in_format_group(self):
        moved = {
            "lint.clang-tidy-config",
            "lint.bazel-groups",
            "lint.tcmalloc-coverage",
            "lint.idl-naming",
            "lint.private-headers",
        }
        actual = {check.spec.check_id for check in CHECKS if check.spec.group == "format"}
        self.assertTrue(moved.isdisjoint(actual))


class SelectionTest(unittest.TestCase):
    def setUp(self):
        self.changed = (CandidateFile("src/a.cpp", ChangeStatus.MODIFIED),)

    def test_matcher_selects_using_old_and_new_rename_paths(self):
        check = _registered(
            "lint.rename",
            matcher=lambda candidate: "old.cpp" in candidate.selection_paths,
        )
        renamed = (CandidateFile("new.txt", ChangeStatus.RENAMED, old_path="old.cpp"),)
        selected = select_checks(renamed, groups=["lint"], checks=(check,))
        self.assertEqual([check.spec.check_id for check in selected.checks], ["lint.rename"])

    def test_deleted_path_can_trigger_but_is_not_passed_to_tool(self):
        deleted = CandidateFile("src/deleted.cpp", ChangeStatus.DELETED, exists=False)
        self.assertEqual(runnable_paths((deleted,)), ())
        self.assertEqual(telemetry_paths((deleted,)), ("src/deleted.cpp",))

    def test_only_forces_check_and_includes_dependencies(self):
        parent = _registered("lint.parent")
        child = _registered("lint.child", dependencies=("lint.parent",), matcher=lambda _: False)
        selected = select_checks((), only=["lint.child"], checks=(parent, child))
        self.assertEqual(
            [check.spec.check_id for check in selected.checks],
            ["lint.parent", "lint.child"],
        )

    def test_skipped_dependency_marks_dependent_unavailable(self):
        parent = _registered("lint.parent")
        child = _registered("lint.child", dependencies=("lint.parent",))
        selected = select_checks(
            self.changed,
            only=["lint.child"],
            skip=["lint.parent"],
            checks=(parent, child),
        )
        self.assertEqual(selected.unavailable_dependencies, {"lint.child": ("lint.parent",)})

    def test_all_selects_every_check_in_requested_group(self):
        yes = _registered("lint.yes")
        no = _registered("format.no", group="format", matcher=lambda _: False)
        selected = select_checks((), groups=["format"], all_files=True, checks=(yes, no))
        self.assertEqual([check.spec.check_id for check in selected.checks], ["format.no"])

    def test_all_still_reports_only_predicate_matched_candidates(self):
        python = CandidateFile("buildscripts/check.py", ChangeStatus.MODIFIED)
        markdown = CandidateFile("README.md", ChangeStatus.MODIFIED)
        check = _registered("lint.python", matcher=lambda candidate: candidate.path.endswith(".py"))
        self.assertEqual(
            matched_candidates(check, (python, markdown), all_files=True),
            (python,),
        )

    def test_unknown_filter_is_usage_error(self):
        with self.assertRaisesRegex(ValueError, "unknown quality-check"):
            select_checks(self.changed, only=["lint.typo"], checks=(_registered("lint.ok"),))

    def test_codeowners_reacts_to_new_deleted_and_renamed_paths(self):
        codeowners = tuple(
            check
            for check in CHECKS
            if check.spec.group == "codeowners" or check.spec.check_id == "bazel.server"
        )
        for candidate in (
            CandidateFile("new.txt", ChangeStatus.ADDED),
            CandidateFile("gone.txt", ChangeStatus.DELETED, exists=False),
            CandidateFile("new-name.txt", ChangeStatus.RENAMED, old_path="old-name.txt"),
        ):
            with self.subTest(status=candidate.status):
                selected = select_checks((candidate,), groups=["codeowners"], checks=codeowners)
                self.assertEqual(
                    [check.spec.check_id for check in selected.checks],
                    ["bazel.server", "codeowners.local", "codeowners.github"],
                )

    def test_extensionless_build_and_clang_tidy_select_expected_checks(self):
        build = select_checks(
            (CandidateFile("src/mongo/BUILD", ChangeStatus.MODIFIED),), groups=["lint"]
        )
        build_ids = {check.spec.check_id for check in build.checks}
        self.assertIn("lint.target-coverage", build_ids)
        self.assertIn("lint.duplicate-library", build_ids)

        clang_tidy = select_checks(
            (CandidateFile("src/mongo/.clang-tidy", ChangeStatus.MODIFIED),), groups=["lint"]
        )
        self.assertIn(
            "lint.clang-tidy-config",
            {check.spec.check_id for check in clang_tidy.checks},
        )

    def test_bazel_backed_checks_run_after_bazel_server_preflight(self):
        selected = select_checks(
            (CandidateFile("src/mongo/db/example.cpp", ChangeStatus.MODIFIED),),
            groups=["lint"],
        )
        selected_ids = [check.spec.check_id for check in selected.checks]
        self.assertEqual(selected_ids[0], "bazel.server")
        self.assertIn("lint.quickmongolint", selected_ids)
        server = next(check for check in selected.checks if check.spec.check_id == "bazel.server")
        self.assertEqual(server.spec.phase, CheckPhase.PREFLIGHT_SERIAL)


class CandidateDiscoveryTest(unittest.TestCase):
    def test_name_status_preserves_add_modify_delete_and_rename(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "added.py").touch()
            (root / "modified.py").touch()
            (root / "renamed.py").touch()
            output = "A\0added.py\0M\0modified.py\0D\0deleted.py\0" "R100\0old.py\0renamed.py\0"
            parsed = selection._parse_name_status(root, output)
        self.assertEqual(
            [item.status for item in parsed],
            [
                ChangeStatus.ADDED,
                ChangeStatus.MODIFIED,
                ChangeStatus.DELETED,
                ChangeStatus.RENAMED,
            ],
        )
        self.assertEqual(parsed[-1].old_path, "old.py")
        self.assertFalse(parsed[2].exists)

    def test_explicit_nonexistent_file_is_a_deleted_candidate(self):
        with tempfile.TemporaryDirectory() as temp:
            candidate = selection.explicit_files(Path(temp), ["gone.txt"])[0]
        self.assertEqual(candidate.status, ChangeStatus.DELETED)
        self.assertFalse(candidate.exists)

    def test_merge_preserves_rename_source_through_later_modification(self):
        merged = selection._merge_candidates(
            [
                CandidateFile("new.py", ChangeStatus.RENAMED, old_path="old.py"),
                CandidateFile("new.py", ChangeStatus.MODIFIED),
            ]
        )
        self.assertEqual(merged[0].status, ChangeStatus.RENAMED)
        self.assertEqual(merged[0].selection_paths, ("old.py", "new.py"))

    @mock.patch.object(selection, "resolve_origin_branch", return_value="origin/master")
    @mock.patch.object(selection, "_run_git")
    @mock.patch.object(selection, "all_repository_files", return_value=())
    def test_over_100_commits_falls_back_to_all(self, all_files, run_git, _resolve):
        run_git.return_value = "101\n"
        result = selection.discover_changed_files(Path("."))
        self.assertTrue(result.selected_all)
        self.assertIn("100-commit", result.warning)
        all_files.assert_called_once()


class EngineTest(unittest.TestCase):
    def _run(
        self,
        checks: tuple[RegisteredCheck, ...],
        *,
        candidates: tuple[CandidateFile, ...] | None = None,
        fix: bool = False,
        rediscover=None,
        reselect=None,
        jobs: int = 4,
        verbose: bool = False,
    ):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        candidates = candidates or (CandidateFile("a.py", ChangeStatus.MODIFIED),)
        output = io.StringIO()
        ui = TerminalUI(len(checks), stream=output, show_skipped=True)
        recorder = _TelemetryRecorder()
        engine = QualityChecksEngine(
            context=_context(root, candidates),
            selection=CheckSelection(checks),
            ui=ui,
            telemetry=recorder,
            jobs=jobs,
            verbose=verbose,
            rediscover=rediscover,
            reselect=reselect,
        )
        return engine.run(fix=fix), output.getvalue(), recorder

    def test_fixers_run_once_without_verification(self):
        calls = []
        refreshed = (CandidateFile("fixed.py", ChangeStatus.MODIFIED),)

        def runner(context, files):
            calls.append((context.fix, files))
            return RunOutcome(0)

        check = _registered("format.one", group="format", supports_fix=True, runner=runner)
        result, _output, _telemetry = self._run((check,), fix=True, rediscover=lambda: refreshed)
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(calls, [(True, ("a.py",))])
        self.assertEqual([item.operation for item in result.results], ["fix"])

    def test_preflight_runs_before_fixers_and_is_not_repeated(self):
        calls = []

        def runner(context, _files):
            calls.append(context.fix)
            return RunOutcome(0)

        preflight = _registered(
            "bazel.server",
            group="bazel",
            phase=CheckPhase.PREFLIGHT_SERIAL,
            runner=runner,
        )
        fixer = _registered(
            "format.one",
            group="format",
            supports_fix=True,
            dependencies=("bazel.server",),
            runner=runner,
        )
        result, _output, _telemetry = self._run((preflight, fixer), fix=True)

        self.assertEqual(result.exit_code, 0)
        self.assertEqual(calls, [False, True])
        self.assertEqual(
            [item.operation for item in result.results],
            ["check", "fix"],
        )

    def test_fix_failure_blocks_dependent_verification(self):
        calls = []
        fixer = _registered(
            "lint.fixer",
            supports_fix=True,
            runner=lambda *_: (calls.append("fixer") or RunOutcome(1)),
        )
        dependent = _registered(
            "lint.dependent",
            dependencies=("lint.fixer",),
            runner=lambda *_: (calls.append("dependent") or RunOutcome(0)),
        )

        result, _output, _telemetry = self._run((fixer, dependent), fix=True)

        self.assertEqual(result.exit_code, 1)
        self.assertEqual(calls, ["fixer"])
        self.assertEqual(
            [(item.check_id, item.operation, item.status) for item in result.results],
            [
                ("lint.fixer", "fix", CheckStatus.FAIL),
                ("lint.dependent", "check", CheckStatus.SKIPPED),
            ],
        )

    def test_fix_rediscovery_reselects_newly_applicable_checks(self):
        calls = []
        refreshed = (CandidateFile("new.BUILD.bazel", ChangeStatus.ADDED),)
        fixer = _registered(
            "format.fixer",
            group="format",
            supports_fix=True,
            matcher=lambda candidate: candidate.path == "a.py",
            runner=lambda *_: (calls.append("fixer") or RunOutcome(0)),
        )
        added = _registered(
            "lint.added",
            matcher=lambda candidate: candidate.path.endswith(".bazel"),
            runner=lambda *_: (calls.append("added") or RunOutcome(0)),
        )

        def reselect(candidates):
            return select_checks(candidates, checks=(fixer, added))

        result, _output, _telemetry = self._run(
            (fixer,), fix=True, rediscover=lambda: refreshed, reselect=reselect
        )
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(calls, ["fixer", "added"])
        self.assertEqual(
            [(item.check_id, item.operation) for item in result.results],
            [("format.fixer", "fix"), ("lint.added", "check")],
        )

    def test_check_mode_preserves_repository_state(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        tracked = root / "tracked.txt"
        tracked.write_text("before", encoding="utf-8")

        def runner(context, _files):
            if context.fix:
                tracked.write_text("after", encoding="utf-8")
            return RunOutcome(0)

        check = _registered("format.safe", group="format", supports_fix=True, runner=runner)
        ui = TerminalUI(1, stream=io.StringIO())
        engine = QualityChecksEngine(
            context=_context(root, (CandidateFile("tracked.txt", ChangeStatus.MODIFIED),)),
            selection=CheckSelection((check,)),
            ui=ui,
            telemetry=_TelemetryRecorder(),
        )
        self.assertEqual(engine.run(fix=False).exit_code, 0)
        self.assertEqual(tracked.read_text(encoding="utf-8"), "before")

    def test_independent_checks_continue_after_failure(self):
        calls = []
        fail = _registered(
            "lint.fail", runner=lambda *_: (calls.append("fail") or RunOutcome(1, "bad"))
        )
        passed = _registered("lint.pass", runner=lambda *_: (calls.append("pass") or RunOutcome(0)))
        result, output, _telemetry = self._run((fail, passed))
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(calls, ["fail", "pass"])
        self.assertIn("ERROR: FAIL: lint.fail", output)
        self.assertIn("bad", output)

    def test_failed_dependency_only_skips_dependent(self):
        parent = _registered("lint.parent", runner=lambda *_: RunOutcome(1))
        child = _registered("lint.child", dependencies=("lint.parent",))
        independent = _registered("lint.other")
        result, _output, _telemetry = self._run((parent, child, independent))
        statuses = {item.check_id: item.status for item in result.results}
        self.assertEqual(statuses["lint.child"], CheckStatus.SKIPPED)
        self.assertEqual(statuses["lint.other"], CheckStatus.PASS)

    def test_adapter_exception_is_orchestration_failure(self):
        def explode(*_args):
            raise RuntimeError("boom")

        result, output, _telemetry = self._run((_registered("lint.bad", runner=explode),))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("RuntimeError", output)

    def test_safe_checks_overlap_up_to_worker_limit(self):
        lock = threading.Lock()
        release = threading.Event()
        running = 0
        maximum = 0

        def runner(*_args):
            nonlocal running, maximum
            with lock:
                running += 1
                maximum = max(maximum, running)
                if running == 2:
                    release.set()
            release.wait(1)
            with lock:
                running -= 1
            return RunOutcome(0)

        checks = tuple(
            _registered(f"lint.parallel-{index}", phase=CheckPhase.SAFE_PARALLEL, runner=runner)
            for index in range(2)
        )
        result, output, _telemetry = self._run(checks, jobs=2)
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(maximum, 2)
        self.assertLess(
            output.index("PASS: lint.parallel-0"), output.index("PASS: lint.parallel-1")
        )

    def test_same_phase_parallel_dependency_waits_and_skips_after_failure(self):
        calls = []
        parent = _registered(
            "lint.parent",
            phase=CheckPhase.SAFE_PARALLEL,
            runner=lambda *_: (calls.append("parent") or RunOutcome(1)),
        )
        child = _registered(
            "lint.child",
            phase=CheckPhase.SAFE_PARALLEL,
            dependencies=("lint.parent",),
            runner=lambda *_: (calls.append("child") or RunOutcome(0)),
        )
        result, _output, _telemetry = self._run((parent, child), jobs=2)
        self.assertEqual(calls, ["parent"])
        self.assertEqual(result.results[-1].status, CheckStatus.SKIPPED)

    def test_success_output_is_buffered_unless_verbose(self):
        check = _registered("lint.chatty", runner=lambda *_: RunOutcome(0, "hello\n"))
        _result, quiet, _telemetry = self._run((check,))
        _result, verbose, _telemetry = self._run((check,), verbose=True)
        self.assertNotIn("hello", quiet)
        self.assertIn("hello", verbose)

    def test_telemetry_failure_does_not_change_check_result(self):
        class BrokenTelemetry:
            def record_result(self, _result):
                raise RuntimeError("telemetry unavailable")

        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        check = _registered("lint.ok")
        engine = QualityChecksEngine(
            context=_context(Path(temp.name), (CandidateFile("a.py", ChangeStatus.MODIFIED),)),
            selection=CheckSelection((check,)),
            ui=TerminalUI(1, stream=io.StringIO()),
            telemetry=BrokenTelemetry(),
        )
        self.assertEqual(engine.run(fix=False).exit_code, 0)


class ProgressTest(unittest.TestCase):
    def test_non_tty_output_is_deterministic(self):
        stream = io.StringIO()
        ui = TerminalUI(1, stream=stream)
        spec = CheckSpec("lint.a", "A", "lint", CheckPhase.SHARED_STATE_SERIAL)
        ui.record_check_started(spec)
        from buildscripts.quality_checks.models import CheckResult

        ui.record_result(CheckResult(spec, CheckStatus.PASS, 0.25, time.time_ns()))
        self.assertEqual(stream.getvalue().splitlines(), ["RUNNING: A", "PASS: A (0.2s)"])

    def test_all_skipped_reports_no_applicable_checks(self):
        stream = io.StringIO()
        ui = TerminalUI(1, stream=stream)
        spec = CheckSpec("lint.a", "A", "lint", CheckPhase.SHARED_STATE_SERIAL)
        ui.record_skipped_check(spec, "no files")
        ui.print_final_summary()
        self.assertEqual(stream.getvalue(), "No applicable checks.\n")


class AdapterTest(unittest.TestCase):
    def test_progress_stream_is_not_captured_with_in_process_tool_output(self):
        with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as terminal:
            saved_stderr_fd = os.dup(2)
            try:
                os.dup2(terminal.fileno(), 2)
                progress = cli._duplicate_progress_stream(sys.stderr)
                assert progress is not None

                def tool() -> bool:
                    print("tool output")
                    progress.write("RUNNING: visible progress\n")
                    progress.flush()
                    return True

                result = adapters.run_python_tool(tool)
                progress.close()
            finally:
                os.dup2(saved_stderr_fd, 2)
                os.close(saved_stderr_fd)

            terminal.seek(0)
            progress_output = terminal.read()

        self.assertIn("tool output", result.output)
        self.assertNotIn("RUNNING", result.output)
        self.assertIn("RUNNING: visible progress", progress_output)

    @mock.patch.object(adapters, "run_subprocess", return_value=RunOutcome(0))
    def test_bazel_server_check_forwards_startup_options(self, run):
        with tempfile.TemporaryDirectory() as temp:
            context = _context(Path(temp), ())
            context.bazel_real = "/opt/bazel"
            context.bazel_startup_options = ("--output_base=/tmp/bazel",)
            result = adapters.run_bazel_server(context, ())

        self.assertEqual(result.returncode, 0)
        run.assert_called_once_with(
            ["/opt/bazel", "--output_base=/tmp/bazel", "info", "server_pid"],
            cwd=Path(temp),
        )

    @mock.patch.object(adapters, "run_subprocess", return_value=RunOutcome(0))
    def test_format_check_uses_repeated_files_without_mutating(self, run):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "a.py").touch()
            (root / "b.js").touch()
            context = _context(root, ())
            adapters.run_format(context, ("a.py", "b.js"))
        command = run.call_args.args[0]
        self.assertIn("--check", command)
        self.assertEqual(command.count("--file"), 2)

    @mock.patch.object(adapters, "run_subprocess", return_value=RunOutcome(0))
    def test_format_fix_omits_check_flag(self, run):
        with tempfile.TemporaryDirectory() as temp:
            context = _context(Path(temp), ())
            context.fix = True
            context.all_files = True
            adapters.run_format(context, ())
        self.assertNotIn("--check", run.call_args.args[0])
        self.assertIn("--all", run.call_args.args[0])

    def test_windows_unsupported_checks_are_skipped(self):
        with tempfile.TemporaryDirectory() as temp:
            context = _context(Path(temp), ())
            with mock.patch.object(adapters.platform, "system", return_value="Windows"):
                checks = (
                    (adapters.run_format, "repository formatters are unsupported on Windows"),
                    (adapters.run_quickmongolint, "quickmongolint is unsupported on Windows"),
                    (adapters.run_pyright, "pyright is unsupported on Windows"),
                    (
                        adapters.run_target_coverage,
                        "Bazel target coverage is unsupported on Windows",
                    ),
                    (
                        adapters.run_module_lockfile,
                        "MODULE.bazel.lock validation is unsupported on Windows",
                    ),
                    (adapters.run_combined_rules_lint, "rules_lint is unsupported on Windows"),
                    (
                        adapters.run_yaml,
                        "Evergreen and YAML validation is unsupported on Windows",
                    ),
                    (
                        adapters.run_sbom,
                        "SBOM validation is unsupported on Windows (jsonschema_specifications' "
                        "nested schema resources exceed Windows MAX_PATH); SBOM generation only "
                        "runs in Evergreen on Linux",
                    ),
                )
                for runner, reason in checks:
                    with self.subTest(reason=reason):
                        result = runner(context, ())
                        self.assertEqual(result.returncode, 0)
                        self.assertEqual(result.skipped_reason, reason)

    def test_macos_rules_lint_is_skipped(self):
        with tempfile.TemporaryDirectory() as temp:
            context = _context(Path(temp), ())
            with mock.patch.object(adapters.platform, "system", return_value="Darwin"):
                result = adapters.run_combined_rules_lint(context, ())
        self.assertEqual(result.returncode, 0)
        self.assertEqual(
            result.skipped_reason,
            "rules_lint is unsupported on macOS (jstestfuzz requires Linux)",
        )

    @mock.patch.object(adapters, "run_subprocess", return_value=RunOutcome(0))
    def test_all_check_mode_bazel_runs_use_non_updating_lockfiles(self, run):
        with tempfile.TemporaryDirectory() as temp:
            context = _context(Path(temp), ())
            context.all_files = True
            context.bazel_options = ("--lockfile_mode=update",)
            adapters.run_format(context, ())
        command = run.call_args.args[0]
        self.assertIn("--lockfile_mode=error", command)
        self.assertNotIn("--lockfile_mode=update", command)

    def test_file_size_check_handles_extensionless_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            large = root / "extensionless"
            large.write_bytes(b"x" * 11 * 1024 * 1024)
            result = adapters.run_file_size(_context(root, ()), ("extensionless",))
        self.assertEqual(result.returncode, 1)
        self.assertIn("extensionless", result.output)

    def test_unsupported_codeowners_group_is_skipped(self):
        with tempfile.TemporaryDirectory() as temp:
            context = _context(Path(temp), ())
            with mock.patch.object(adapters, "codeowners_supported", return_value=False):
                result = adapters.run_codeowners(context, ())
        self.assertEqual(result.returncode, 0)
        self.assertIn("codeowners is unsupported on", result.skipped_reason or "")

    def test_rules_lint_is_skipped_on_windows(self):
        with tempfile.TemporaryDirectory() as temp:
            context = _context(Path(temp), ())
            with mock.patch.object(adapters.platform, "system", return_value="Windows"):
                result = adapters.run_combined_rules_lint(context, ())
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.skipped_reason, "rules_lint is unsupported on Windows")

    @mock.patch.object(
        adapters,
        "run_subprocess",
        return_value=RunOutcome(
            0,
            skipped_reason="authentication is unavailable; skipping this check",
        ),
    )
    def test_codeowners_authentication_failure_is_skipped(self, run):
        with tempfile.TemporaryDirectory() as temp:
            context = _context(Path(temp), ())
            context.expansions_file = "expansions.yml"
            result = adapters.run_codeowners_github(context, ())

        self.assertEqual(
            result.skipped_reason, "authentication is unavailable; skipping this check"
        )
        self.assertTrue(run.call_args.kwargs["skip_on_auth_failure"])

    def test_authentication_failures_are_skipped_when_requested(self):
        def fake_run(_command, *, stdout, **_kwargs):
            stdout.write(b"401 Unauthorized: authentication failed\n")
            return subprocess.CompletedProcess([], 1)

        with (
            tempfile.TemporaryDirectory() as temp,
            mock.patch.object(adapters.subprocess, "run", side_effect=fake_run),
        ):
            result = adapters.run_subprocess(
                ["authenticated-tool"], cwd=Path(temp), skip_on_auth_failure=True
            )

        self.assertEqual(result.returncode, 0)
        self.assertEqual(
            result.skipped_reason, "authentication is unavailable; skipping this check"
        )

    def test_large_files_compatibility_flag_checks_changed_candidates_only(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "changed.txt").write_text("small", encoding="utf-8")
            (root / "unrelated.bin").write_bytes(b"x" * 11 * 1024 * 1024)
            context = _context(root, ())
            context.lint_args = ("--large-files",)
            result = adapters.run_file_size(context, ("changed.txt",))
        self.assertEqual(result.returncode, 0)


class TelemetryTest(unittest.TestCase):
    def test_paths_are_relative_deduplicated_and_capped(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            paths = [f"src/{index}.py" for index in range(MAX_PATH_SAMPLES + 7)]
            paths.append("/outside/repository")
            sample = telemetry.sample_repo_relative_paths(root, paths)
        self.assertEqual(sample.total_count, MAX_PATH_SAMPLES + 7)
        self.assertEqual(len(sample.paths), MAX_PATH_SAMPLES)
        self.assertTrue(sample.truncated)
        self.assertTrue(all(not Path(path).is_absolute() for path in sample.paths))

    def test_path_sampling_ignores_invalid_path_values(self):
        with tempfile.TemporaryDirectory() as temp:
            sample = telemetry.sample_repo_relative_paths(Path(temp), ["good.py", "bad\0path"])
        self.assertEqual(sample.paths, ("good.py",))

    @mock.patch.object(telemetry.subprocess, "Popen", side_effect=OSError("no process"))
    def test_export_launch_failure_is_fail_open_and_logs_without_terminal_output(self, _popen):
        with tempfile.TemporaryDirectory() as temp:
            client = TelemetryClient(Path(temp))
            with mock.patch.dict(
                os.environ, {"OTEL_EXPORTER_OTLP_ENDPOINT": "collector"}, clear=False
            ):
                result = client.finish(
                    completed_checks=0,
                    failed_checks=0,
                    serial_checks=0,
                    parallel_checks=0,
                    wallclock_seconds=0,
                    verbose=True,
                )
            assert result is not None
            self.assertIn("Telemetry exporter launch failed", result.read_text(encoding="utf-8"))
            self.assertEqual(list(Path(temp).glob("*.json")), [])

    @mock.patch.object(telemetry.json, "loads", side_effect=ValueError("bad payload"))
    def test_uploader_failure_is_fail_open_and_writes_to_redirected_log(self, _loads):
        with tempfile.TemporaryDirectory() as temp:
            payload_path = Path(temp) / "payload.json"
            payload_path.write_text("{}", encoding="utf-8")
            log = io.StringIO()
            with redirect_stderr(log):
                result = telemetry._run_telemetry_uploader(payload_path)

        self.assertEqual(result, 0)
        self.assertFalse(payload_path.exists())
        self.assertIn("Telemetry exporter payload read failed", log.getvalue())

    def test_disabled_sdk_never_launches_uploader(self):
        with tempfile.TemporaryDirectory() as temp:
            client = TelemetryClient(Path(temp))
            with (
                mock.patch.object(telemetry.subprocess, "Popen") as popen,
                mock.patch.dict(os.environ, {"OTEL_SDK_DISABLED": "true"}, clear=False),
            ):
                self.assertIsNone(
                    client.finish(
                        completed_checks=0,
                        failed_checks=0,
                        serial_checks=0,
                        parallel_checks=0,
                        wallclock_seconds=0,
                    )
                )
                popen.assert_not_called()

    def test_payload_contains_invocation_and_preparation_spans(self):
        with tempfile.TemporaryDirectory() as temp:
            client = TelemetryClient(Path(temp))
            client.record_preparation(duration_seconds=0.1)
            with (
                mock.patch.object(telemetry.subprocess, "Popen") as popen,
                mock.patch.dict(
                    os.environ, {"OTEL_EXPORTER_OTLP_ENDPOINT": "collector"}, clear=False
                ),
            ):
                client.finish(
                    completed_checks=0,
                    failed_checks=0,
                    serial_checks=0,
                    parallel_checks=0,
                    wallclock_seconds=0.1,
                )
                payload_path = Path(popen.call_args.args[0][3])
                payload = json.loads(payload_path.read_text())
                payload_path.unlink()
        self.assertEqual(payload["root"]["name"], "mongo.quality_checks.invocation")
        self.assertEqual(payload["children"][0]["name"], "mongo.quality_checks.preparation")


class CliTest(unittest.TestCase):
    def _alias_mode(self, alias: str) -> tuple[bool, frozenset[str]]:
        candidate = CandidateFile("file.py", ChangeStatus.MODIFIED)
        discovery = DiscoveryResult((candidate,), "origin/master")
        fake_result = EngineResult(0, (), {})
        with (
            mock.patch.object(cli, "_discover", return_value=discovery),
            mock.patch.object(cli, "QualityChecksEngine") as engine_type,
            mock.patch.object(cli, "TelemetryClient") as telemetry_type,
        ):
            engine_type.return_value.run.return_value = fake_result
            telemetry_type.create.return_value.finish.return_value = None
            code = cli.main(
                ["--invocation-name", alias, "--bazel-real", "bazel"],
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        self.assertEqual(code, 0)
        kwargs = engine_type.call_args.kwargs
        return engine_type.return_value.run.call_args.kwargs["fix"], kwargs[
            "context"
        ].explicit_groups

    def test_legacy_alias_defaults(self):
        self.assertTrue(self._alias_mode("format")[0])
        self.assertFalse(self._alias_mode("lint")[0])
        self.assertTrue(self._alias_mode("codeowners")[0])
        self.assertFalse(self._alias_mode("checks")[0])

    def test_all_and_file_is_usage_error(self):
        stderr = io.StringIO()
        with mock.patch.object(cli, "TelemetryClient") as telemetry_type:
            telemetry_type.create.return_value.finish.return_value = None
            code = cli.main(["--all", "--file", "a.py"], stderr=stderr)
        self.assertEqual(code, 2)
        self.assertIn("mutually exclusive", stderr.getvalue())

    def test_lint_labels_are_forwarded_to_compatibility_adapter(self):
        discovery = DiscoveryResult((), "origin/master")
        fake_result = EngineResult(0, (), {})
        with (
            mock.patch.object(cli, "_discover", return_value=discovery),
            mock.patch.object(cli, "QualityChecksEngine") as engine_type,
            mock.patch.object(cli, "TelemetryClient") as telemetry_type,
        ):
            engine_type.return_value.run.return_value = fake_result
            telemetry_type.create.return_value.finish.return_value = None
            code = cli.main(
                ["--invocation-name", "lint", "//src/mongo/..."],
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        self.assertEqual(code, 0)
        context = engine_type.call_args.kwargs["context"]
        self.assertIn("//src/mongo/...", context.lint_args)
        selected = engine_type.call_args.kwargs["selection"]
        self.assertEqual(
            [check.spec.check_id for check in selected.checks],
            ["bazel.server", "lint.rules-lint"],
        )

    def test_legacy_all_lint_label_preserves_all_file_rediscovery_after_fix(self):
        discovery = DiscoveryResult((), "origin/master", selected_all=True)
        fake_result = EngineResult(0, (), {})
        with (
            mock.patch.object(cli, "_discover", return_value=discovery) as discover,
            mock.patch.object(cli, "_selection_needs_buildozer", return_value=False),
            mock.patch.object(cli, "QualityChecksEngine") as engine_type,
            mock.patch.object(cli, "TelemetryClient") as telemetry_type,
        ):
            engine_type.return_value.run.return_value = fake_result
            telemetry_type.create.return_value.finish.return_value = None
            code = cli.main(
                ["--invocation-name", "lint", "--fix", "//..."],
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
            engine_type.call_args.kwargs["rediscover"]()
            self.assertEqual(discover.call_count, 2)
            self.assertTrue(all(call.kwargs["force_all"] for call in discover.call_args_list))
        self.assertEqual(code, 0)
        self.assertTrue(engine_type.call_args.kwargs["context"].all_files)
        selected_checks = engine_type.call_args.kwargs["selection"].checks
        self.assertEqual(selected_checks[0].spec.check_id, "bazel.server")
        self.assertTrue(all(check.spec.group == "lint" for check in selected_checks[1:]))

    def test_unsupported_codeowners_group_is_skipped_without_changes(self):
        discovery = DiscoveryResult((), "origin/master")
        fake_result = EngineResult(0, (), {})
        with (
            mock.patch.object(cli, "_discover", return_value=discovery),
            mock.patch.object(cli, "QualityChecksEngine") as engine_type,
            mock.patch.object(cli, "TelemetryClient") as telemetry_type,
        ):
            engine_type.return_value.run.return_value = fake_result
            telemetry_type.create.return_value.finish.return_value = None
            code = cli.main(
                ["--group", "codeowners"],
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        self.assertEqual(code, 0)
        self.assertEqual(
            [check.spec.check_id for check in engine_type.call_args.kwargs["selection"].checks],
            [],
        )
        self.assertEqual(
            [
                check.spec.check_id
                for check, _reason in engine_type.call_args.kwargs["selection"].skipped
            ],
            ["codeowners.local", "codeowners.github"],
        )

    def test_large_files_forces_repository_file_size_check(self):
        discovery = DiscoveryResult((), "origin/master")
        fake_result = EngineResult(0, (), {})
        with (
            mock.patch.object(cli, "_discover", return_value=discovery),
            mock.patch.object(cli, "QualityChecksEngine") as engine_type,
            mock.patch.object(cli, "TelemetryClient") as telemetry_type,
        ):
            engine_type.return_value.run.return_value = fake_result
            telemetry_type.create.return_value.finish.return_value = None
            code = cli.main(
                ["--invocation-name", "lint", "--large-files"],
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        self.assertEqual(code, 0)
        selected = engine_type.call_args.kwargs["selection"]
        self.assertEqual([check.spec.check_id for check in selected.checks], ["lint.file-size"])

    def test_large_files_does_not_escape_an_explicit_non_lint_group(self):
        discovery = DiscoveryResult((), "origin/master")
        fake_result = EngineResult(0, (), {})
        with (
            mock.patch.object(cli, "_discover", return_value=discovery),
            mock.patch.object(cli, "QualityChecksEngine") as engine_type,
            mock.patch.object(cli, "TelemetryClient") as telemetry_type,
        ):
            engine_type.return_value.run.return_value = fake_result
            telemetry_type.create.return_value.finish.return_value = None
            code = cli.main(
                ["--group", "format", "--large-files"],
                stdout=io.StringIO(),
                stderr=io.StringIO(),
            )
        self.assertEqual(code, 0)
        self.assertEqual(engine_type.call_args.kwargs["selection"].checks, ())


if __name__ == "__main__":
    unittest.main()
