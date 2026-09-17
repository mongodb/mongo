"""Adapters from the quality-checks engine to existing MongoDB tools."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import tempfile
import threading
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from buildscripts.quality_checks.models import CandidateFile


@dataclass(frozen=True, slots=True)
class RunOutcome:
    returncode: int
    output: str = ""
    skipped_reason: str | None = None
    fatal: bool = False


_AUTHENTICATION_FAILURE_MARKERS = (
    "authentication failed",
    "not authenticated",
    "unauthorized",
    "bad credentials",
    "no github token",
    "login required",
    "oauth token",
    "invalid token",
    "expired token",
    "401",
    "403",
)


class SharedTools:
    """Resolve expensive shared tools at most once per invocation."""

    def __init__(self) -> None:
        self._buildozer: str | None = None
        self._lint_files_with_targets: list[str] | None = None
        self._lint_files_key: tuple[str, tuple[str, ...], tuple[str, ...]] | None = None
        self._lock = threading.Lock()

    @property
    def buildozer(self) -> str:
        with self._lock:
            if self._buildozer is None:
                from buildscripts.install_bazel import install_buildozer

                binary_name = "buildozer.exe" if platform.system() == "Windows" else "buildozer"
                existing = shutil.which(binary_name)
                if existing is None:
                    install_dir = Path.home() / ".local" / "bin"
                    install_dir.mkdir(parents=True, exist_ok=True)
                    existing = install_buildozer(str(install_dir))
                if existing is None:
                    raise RuntimeError("buildozer is not supported on this platform")
                self._buildozer = existing
            return self._buildozer

    def lint_files_with_targets(
        self,
        bazel_bin: str,
        bazel_options: Sequence[str],
        bazel_startup_options: Sequence[str],
    ) -> list[str]:
        """Cache the expensive whole-repository source-target query per phase."""

        key = (bazel_bin, tuple(bazel_options), tuple(bazel_startup_options))
        with self._lock:
            if self._lint_files_with_targets is None or self._lint_files_key != key:
                from bazel.wrapper_hook.lint import list_files_with_targets

                self._lint_files_with_targets = list_files_with_targets(
                    bazel_bin, bazel_options, bazel_startup_options
                )
                self._lint_files_key = key
            return list(self._lint_files_with_targets)

    def clear_lint_state(self) -> None:
        """Invalidate candidate-dependent shared results after fixers run."""

        with self._lock:
            self._lint_files_with_targets = None
            self._lint_files_key = None


@dataclass(slots=True)
class CheckContext:
    repo_root: Path
    bazel_real: str
    candidates: tuple[CandidateFile, ...]
    all_files: bool
    fix: bool
    origin_branch: str
    invocation_name: str
    explicit_groups: frozenset[str]
    lint_args: tuple[str, ...] = ()
    format_args: tuple[str, ...] = ()
    codeowners_args: tuple[str, ...] = ()
    bazel_startup_options: tuple[str, ...] = ()
    bazel_options: tuple[str, ...] = ()
    expansions_file: str | None = None
    codeowners_branch: str | None = None
    is_ci: bool = False
    shared_tools: SharedTools = field(default_factory=SharedTools)


def _read_output(stream: object) -> str:
    stream.seek(0)
    return stream.read().decode("utf-8", errors="replace")


def _is_authentication_failure(output: str) -> bool:
    normalized = output.casefold()
    return any(marker in normalized for marker in _AUTHENTICATION_FAILURE_MARKERS)


def run_subprocess(
    command: list[str], *, cwd: Path, skip_on_auth_failure: bool = False
) -> RunOutcome:
    with tempfile.TemporaryFile(mode="w+b") as output:
        try:
            result = subprocess.run(
                command, cwd=cwd, check=False, stdout=output, stderr=subprocess.STDOUT
            )
            captured = _read_output(output)
            if (
                result.returncode != 0
                and skip_on_auth_failure
                and _is_authentication_failure(captured)
            ):
                return RunOutcome(
                    0,
                    captured,
                    skipped_reason="authentication is unavailable; skipping this check",
                )
            return RunOutcome(result.returncode, captured)
        except OSError as exc:
            return RunOutcome(2, f"Failed to execute {command[0]}: {exc}\n", fatal=True)


def run_python_tool(callback: Callable[[], bool | None]) -> RunOutcome:
    """Capture Python and child-process output for a legacy in-process tool."""

    old_stdout_fd = os.dup(1)
    old_stderr_fd = os.dup(2)
    with tempfile.TemporaryFile(mode="w+b") as output:
        try:
            sys.stdout.flush()
            sys.stderr.flush()
            os.dup2(output.fileno(), 1)
            os.dup2(output.fileno(), 2)
            try:
                success = callback()
                returncode = 0 if success is not False else 1
            except SystemExit as exc:
                returncode = int(exc.code) if isinstance(exc.code, int) else 1
            except Exception as exc:
                import traceback

                traceback.print_exc()
                print(f"quality check raised {type(exc).__name__}: {exc}", file=sys.stderr)
                returncode = 1
            finally:
                sys.stdout.flush()
                sys.stderr.flush()
                os.dup2(old_stdout_fd, 1)
                os.dup2(old_stderr_fd, 2)
            return RunOutcome(returncode, _read_output(output))
        finally:
            os.close(old_stdout_fd)
            os.close(old_stderr_fd)


def _effective_bazel_options(context: CheckContext) -> list[str]:
    options = list(context.bazel_options)
    if context.fix:
        return options

    # Check mode always wins over an explicitly supplied updating mode.
    filtered: list[str] = []
    skip_value = False
    for option in options:
        if skip_value:
            skip_value = False
            continue
        if option == "--lockfile_mode":
            skip_value = True
            continue
        if option.startswith("--lockfile_mode="):
            continue
        filtered.append(option)
    filtered.append("--lockfile_mode=error")
    return filtered


def _bazel_run_command(context: CheckContext, target: str, tool_args: list[str]) -> list[str]:
    bazel_options = _effective_bazel_options(context)
    command = [
        context.bazel_real,
        *context.bazel_startup_options,
        "run",
        *bazel_options,
        target,
    ]
    if tool_args:
        command.extend(["--", *tool_args])
    return command


def run_bazel_server(context: CheckContext, _matched_files: tuple[str, ...]) -> RunOutcome:
    """Start the Bazel server, if necessary, before Bazel-backed checks run."""

    return run_subprocess(
        [context.bazel_real, *context.bazel_startup_options, "info", "server_pid"],
        cwd=context.repo_root,
    )


def run_format(context: CheckContext, matched_files: tuple[str, ...]) -> RunOutcome:
    if platform.system() == "Windows":
        return RunOutcome(
            0,
            skipped_reason="repository formatters are unsupported on Windows",
        )
    if not context.all_files and not matched_files:
        return RunOutcome(0, skipped_reason="no existing matched files to format")
    tool_args = [*context.format_args, "--origin-branch", context.origin_branch]
    if not context.fix:
        tool_args.append("--check")
    if context.all_files:
        tool_args.append("--all")
    else:
        for path in matched_files:
            if (context.repo_root / path).is_file():
                tool_args.extend(["--file", path])
    return run_subprocess(
        _bazel_run_command(context, "//:format", tool_args), cwd=context.repo_root
    )


def _is_build_file(path: str) -> bool:
    return path.endswith((".bazel", ".bzl")) or Path(path).name in {"BUILD", "BUILD.bazel"}


def run_lint(context: CheckContext, matched_files: tuple[str, ...]) -> RunOutcome:
    from bazel.wrapper_hook.lint import LinterFail, run_rules_lint

    lint_args = [*context.lint_args]
    lint_args.extend(["--origin-branch", context.origin_branch, "--keep-going"])
    if context.fix:
        lint_args.append("--fix")
    if context.all_files:
        lint_args.extend(["--all", "//..."])
    bazel_options = _effective_bazel_options(context)
    buildozer = None
    if context.all_files or any(_is_build_file(path) for path in matched_files):
        buildozer = context.shared_tools.buildozer

    skipped_reason: str | None = None

    def invoke() -> bool:
        nonlocal skipped_reason
        try:
            skipped_reason = run_rules_lint(
                context.bazel_real,
                lint_args,
                candidate_files=None if context.all_files else list(matched_files),
                lint_all_override=context.all_files,
                buildozer=buildozer,
                bazel_options=bazel_options,
                bazel_startup_options=context.bazel_startup_options,
                files_with_targets_provider=lambda: context.shared_tools.lint_files_with_targets(
                    context.bazel_real, bazel_options, context.bazel_startup_options
                ),
            )
            return True
        except (LinterFail, subprocess.CalledProcessError) as exc:
            print(str(exc), file=sys.stderr)
            return False

    outcome = run_python_tool(invoke)
    if skipped_reason is not None:
        return RunOutcome(0, outcome.output, skipped_reason=skipped_reason)
    return outcome


def _run_lint_stage(
    context: CheckContext, matched_files: tuple[str, ...], stage: str
) -> RunOutcome:
    from bazel.wrapper_hook.lint import LinterFail, run_rules_lint

    if platform.system() == "Windows":
        unsupported_reasons = {
            "module-lockfile": "MODULE.bazel.lock validation is unsupported on Windows",
            "pyright": "pyright is unsupported on Windows",
            "quickmongolint": "quickmongolint is unsupported on Windows",
            "target-coverage": "Bazel target coverage is unsupported on Windows",
            "rules-lint": "rules_lint is unsupported on Windows",
            "yaml": "Evergreen and YAML validation is unsupported on Windows",
            "sbom": (
                "SBOM validation is unsupported on Windows (jsonschema_specifications' "
                "nested schema resources exceed Windows MAX_PATH); SBOM generation only "
                "runs in Evergreen on Linux"
            ),
        }
        if stage in unsupported_reasons:
            return RunOutcome(0, skipped_reason=unsupported_reasons[stage])
    elif platform.system() == "Darwin" and stage == "rules-lint":
        return RunOutcome(
            0,
            skipped_reason="rules_lint is unsupported on macOS (jstestfuzz requires Linux)",
        )

    lint_args = [*context.lint_args, "--origin-branch", context.origin_branch, "--keep-going"]
    if context.fix:
        lint_args.append("--fix")
    if context.all_files:
        lint_args.extend(["--all", "//..."])
    bazel_options = _effective_bazel_options(context)
    buildozer = context.shared_tools.buildozer if stage == "duplicate-library" else None

    skipped_reason: str | None = None

    def invoke() -> bool:
        nonlocal skipped_reason
        try:
            skipped_reason = run_rules_lint(
                context.bazel_real,
                lint_args,
                candidate_files=None if context.all_files else list(matched_files),
                lint_all_override=context.all_files,
                buildozer=buildozer,
                bazel_options=bazel_options,
                bazel_startup_options=context.bazel_startup_options,
                enabled_checks=frozenset({stage}),
                files_with_targets_provider=lambda: context.shared_tools.lint_files_with_targets(
                    context.bazel_real, bazel_options, context.bazel_startup_options
                ),
            )
            return True
        except (LinterFail, subprocess.CalledProcessError) as exc:
            print(str(exc), file=sys.stderr)
            return False

    outcome = run_python_tool(invoke)
    if skipped_reason is not None:
        return RunOutcome(0, outcome.output, skipped_reason=skipped_reason)
    return outcome


def run_target_coverage(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "target-coverage")


def run_sbom(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "sbom")


def run_quickmongolint(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "quickmongolint")


def run_errorcodes(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "errorcodes")


def run_pyright(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "pyright")


def run_module_lockfile(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "module-lockfile")


def run_uv_lockfile(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "uv-lockfile")


def run_generated_evergreen(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "generated-evergreen")


def run_copybara_forbidden_text(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "copybara-forbidden-text")


def run_yaml(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "yaml")


def run_resmoke_tags(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "resmoke-tags")


def run_streams_coverage(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "streams-coverage")


def run_markdown_links(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "markdown-links")


def run_file_size(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    from bazel.wrapper_hook.lint import LARGE_FILE_THRESHOLD

    exclude_third_party = context.all_files or "--large-files" in context.lint_args

    violations = []
    for path in files:
        if exclude_third_party and path.startswith("src/third_party/"):
            continue
        absolute = context.repo_root / path
        try:
            size = absolute.stat().st_size
        except OSError:
            continue
        if size > LARGE_FILE_THRESHOLD:
            violations.append(f"File {path} exceeds large file threshold of {LARGE_FILE_THRESHOLD}")
    if violations:
        return RunOutcome(1, "\n".join(violations) + "\n")
    return RunOutcome(0)


def run_module_mapping(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "module-mapping")


def run_duplicate_library(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "duplicate-library")


def run_combined_rules_lint(context: CheckContext, files: tuple[str, ...]) -> RunOutcome:
    return _run_lint_stage(context, files, "rules-lint")


def _run_validator(
    context: CheckContext,
    callback: Callable[..., bool],
    *,
    needs_buildozer: bool = False,
) -> RunOutcome:
    # Legacy CI reports are written into the checkout. The unified runner
    # reports failures through its structured result and telemetry instead.
    kwargs: dict[str, object] = {"generate_report": False, "fix": context.fix}
    if needs_buildozer:
        kwargs["buildozer"] = context.shared_tools.buildozer
    if callback.__name__ in {
        "validate_bazel_groups",
        "validate_tcmalloc_cc_test_coverage",
        "validate_idl_naming",
    }:
        kwargs["bazel_bin"] = context.bazel_real
        kwargs["bazel_startup_options"] = context.bazel_startup_options
        kwargs["bazel_options"] = _effective_bazel_options(context)
    return run_python_tool(lambda: callback(**kwargs))


def run_clang_tidy_config(context: CheckContext, _matched: tuple[str, ...]) -> RunOutcome:
    from buildscripts.bazel_custom_formatter import validate_clang_tidy_configs

    return _run_validator(context, validate_clang_tidy_configs, needs_buildozer=True)


def run_bazel_groups(context: CheckContext, _matched: tuple[str, ...]) -> RunOutcome:
    from buildscripts.bazel_custom_formatter import validate_bazel_groups

    return _run_validator(context, validate_bazel_groups, needs_buildozer=True)


def run_tcmalloc_coverage(context: CheckContext, _matched: tuple[str, ...]) -> RunOutcome:
    from buildscripts.bazel_custom_formatter import validate_tcmalloc_cc_test_coverage

    return _run_validator(context, validate_tcmalloc_cc_test_coverage)


def run_idl_naming(context: CheckContext, _matched: tuple[str, ...]) -> RunOutcome:
    from buildscripts.bazel_custom_formatter import validate_idl_naming

    return _run_validator(context, validate_idl_naming)


def run_private_headers(context: CheckContext, _matched: tuple[str, ...]) -> RunOutcome:
    from buildscripts.bazel_custom_formatter import validate_private_headers

    return _run_validator(context, validate_private_headers, needs_buildozer=True)


def codeowners_supported() -> bool:
    return platform.system() in {"Linux", "Darwin"} and platform.machine().lower() in {
        "x86_64",
        "amd64",
        "arm64",
        "aarch64",
    }


def _unsupported_codeowners(context: CheckContext) -> RunOutcome | None:
    if codeowners_supported():
        return None
    reason = f"codeowners is unsupported on {platform.system()} {platform.machine()}"
    return RunOutcome(0, skipped_reason=reason)


def run_codeowners(context: CheckContext, _matched_files: tuple[str, ...]) -> RunOutcome:
    if unsupported := _unsupported_codeowners(context):
        return unsupported
    tool_args = list(context.codeowners_args)
    if not context.fix:
        tool_args.append("--check")
    if context.expansions_file:
        tool_args.extend(["--expansions-file", context.expansions_file])
    if context.codeowners_branch:
        tool_args.extend(["--branch", context.codeowners_branch])
    return run_subprocess(
        _bazel_run_command(context, "//:codeowners", tool_args),
        cwd=context.repo_root,
        skip_on_auth_failure=True,
    )


def run_codeowners_github(context: CheckContext, _matched_files: tuple[str, ...]) -> RunOutcome:
    if unsupported := _unsupported_codeowners(context):
        return unsupported
    if not context.expansions_file:
        reason = "GitHub CODEOWNERS validation requires --expansions-file"
        return RunOutcome(0, skipped_reason=reason + "; skipping outside CI")
    return run_subprocess(
        _bazel_run_command(
            context,
            "@bazel_rules_mongo//codeowners:check_github_codeowner_errors",
            ["--expansions-file", context.expansions_file],
        ),
        cwd=context.repo_root,
        skip_on_auth_failure=True,
    )
