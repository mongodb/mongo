"""Tests for the quality-check Bazel pseudo-target adapter."""

from __future__ import annotations

import contextlib
import io
import pathlib
import subprocess
import unittest
from unittest import mock

from bazel.wrapper_hook import quality_checks


class QualityChecksInvocationTest(unittest.TestCase):
    def test_only_handles_run_pseudo_targets(self):
        self.assertIsNone(quality_checks._quality_checks_invocation(["build", "checks"]))
        self.assertIsNone(quality_checks._quality_checks_invocation(["run", "//foo:checks"]))
        self.assertIsNone(quality_checks._quality_checks_invocation(["build", "run", "lint"]))
        self.assertIsNone(
            quality_checks._quality_checks_invocation(["run", "--config", "lint", "//foo:tool"])
        )

    def test_accepts_all_supported_target_spellings(self):
        for name in ("checks", "format", "lint", "codeowners"):
            for target in (name, f":{name}", f"//:{name}"):
                with self.subTest(target=target):
                    self.assertEqual(
                        quality_checks._quality_checks_invocation(["run", target]),
                        (name, [], [], []),
                    )

    def test_separates_bazel_options_from_program_arguments(self):
        self.assertEqual(
            quality_checks._quality_checks_invocation(
                ["run", "--config=local", "checks", "--ui_event_filters=-info", "--", "--all"]
            ),
            (
                "checks",
                ["--all"],
                [],
                ["--config=local", "--ui_event_filters=-info"],
            ),
        )

    def test_skips_common_separate_command_option_values_before_target(self):
        for option, value in (
            ("-j", "4"),
            ("--jobs", "4"),
            ("-c", "opt"),
            ("--compilation_mode", "opt"),
        ):
            with self.subTest(option=option):
                self.assertEqual(
                    quality_checks._quality_checks_invocation(["run", option, value, "checks"]),
                    ("checks", [], [], [option, value]),
                )

    def test_legacy_arguments_without_separator_are_forwarded(self):
        self.assertEqual(
            quality_checks._quality_checks_invocation(["run", "lint", "--fix", "//src/..."]),
            ("lint", ["--fix", "//src/..."], [], []),
        )

    def test_trailing_bazel_options_without_separator_are_not_tool_arguments(self):
        self.assertEqual(
            quality_checks._quality_checks_invocation(
                ["run", "format", "--config=local", "--check"]
            ),
            ("format", ["--check"], [], ["--config=local"]),
        )
        self.assertEqual(
            quality_checks._quality_checks_invocation(
                ["run", "lint", "--config", "local", "--fix", "//src/..."]
            ),
            ("lint", ["--fix", "//src/..."], [], ["--config", "local"]),
        )
        self.assertEqual(
            quality_checks._quality_checks_invocation(
                ["run", "lint", "--local_resources=cpu=HOST_CPUS*.5", "--fix"]
            ),
            ("lint", ["--fix"], [], ["--local_resources=cpu=HOST_CPUS*.5"]),
        )

    def test_legacy_group_value_named_like_an_alias_is_forwarded(self):
        self.assertEqual(
            quality_checks._quality_checks_invocation(["run", "checks", "--group", "lint"]),
            ("checks", ["--group", "lint"], [], []),
        )

    def test_lint_preparation_follows_effective_filters_not_alias_name(self):
        cases = (
            (["run", "format"], False),
            (["run", "format", "--", "--only", "lint.target-coverage"], True),
            (["run", "codeowners", "--", "--group=lint"], True),
            (
                ["run", "format", "--", "--only", "format.formatters, lint.target-coverage"],
                True,
            ),
            (["run", "lint", "--", "--group", "format"], False),
            (["run", "checks"], True),
            (["run", "checks", "--", "--list"], False),
            (["run", "checks", "--", "--help"], False),
            (["run", "checks", "--", "-h"], False),
        )
        for args, expected in cases:
            with self.subTest(args=args):
                invocation = quality_checks._quality_checks_invocation(args)
                assert invocation is not None
                self.assertEqual(quality_checks._invocation_may_run_lint(invocation), expected)


class RunQualityChecksTest(unittest.TestCase):
    def setUp(self):
        self.venv = pathlib.Path("/repo/python3-venv")
        self.runner = mock.Mock(return_value=subprocess.CompletedProcess([], 0))
        self.patches = [
            mock.patch.object(quality_checks, "_target_venv", return_value=self.venv),
            mock.patch.object(
                quality_checks,
                "_venv_python",
                return_value=self.venv / "bin" / "python3",
            ),
            mock.patch.object(quality_checks, "get_terminal_stream", return_value=None),
        ]
        for patcher in self.patches:
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_invokes_repository_venv_with_alias_and_real_bazel(self):
        response = quality_checks.run_quality_checks(
            "/opt/bazel",
            ["run", "//:format", "--", "--check", "--all"],
            runner=self.runner,
        )

        self.assertEqual(response, quality_checks.QualityChecksResponse(True, 0))
        command = self.runner.call_args.args[0]
        self.assertEqual(command[0], str(self.venv / "bin" / "python3"))
        self.assertEqual(
            command[2:6],
            ["--invocation-name", "format", "--bazel-real", "/opt/bazel"],
        )
        self.assertEqual(command[6:], ["--check", "--all"])
        self.assertEqual(self.runner.call_args.kwargs["cwd"], quality_checks.REPO_ROOT)

    def test_forwards_startup_and_command_options(self):
        quality_checks.run_quality_checks(
            "/opt/bazel",
            ["--output_base=/tmp/bazel", "run", "--config=local", "checks", "--", "--all"],
            extra_bazel_options=["--//bazel/config:build_atlas=False"],
            runner=self.runner,
        )

        command = self.runner.call_args.args[0]
        self.assertIn("--bazel-startup-option=--output_base=/tmp/bazel", command)
        self.assertIn("--bazel-option=--config=local", command)
        self.assertIn("--bazel-option=--//bazel/config:build_atlas=False", command)

    def test_does_not_invoke_runner_for_real_bazel_target(self):
        response = quality_checks.run_quality_checks(
            "/opt/bazel", ["run", "//src:mongod"], runner=self.runner
        )

        self.assertFalse(response.handled)
        self.runner.assert_not_called()

    def test_preserves_lint_failure_exit_code_compatibility(self):
        self.runner.return_value = subprocess.CompletedProcess([], 1)
        response = quality_checks.run_quality_checks(
            "/opt/bazel", ["run", "lint"], runner=self.runner
        )
        self.assertEqual(response.exit_code, 3)

    def test_converts_signal_return_codes(self):
        self.runner.return_value = subprocess.CompletedProcess([], -15)
        response = quality_checks.run_quality_checks(
            "/opt/bazel", ["run", "checks"], runner=self.runner
        )
        self.assertEqual(response.exit_code, 143)

    def test_keyboard_interrupt_is_still_a_handled_command(self):
        self.runner.side_effect = KeyboardInterrupt
        response = quality_checks.run_quality_checks(
            "/opt/bazel", ["run", "checks"], runner=self.runner
        )
        self.assertEqual(response, quality_checks.QualityChecksResponse(True, 130))

    def test_start_failure_is_an_orchestration_error(self):
        self.runner.side_effect = OSError("missing interpreter")
        with contextlib.redirect_stderr(io.StringIO()):
            response = quality_checks.run_quality_checks(
                "/opt/bazel", ["run", "checks"], runner=self.runner
            )
        self.assertEqual(response, quality_checks.QualityChecksResponse(True, 2))


if __name__ == "__main__":
    unittest.main()
