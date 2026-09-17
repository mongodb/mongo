"""MongoDB quality-check registration and file triggers."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from buildscripts.quality_checks import adapters
from buildscripts.quality_checks.adapters import CheckContext, RunOutcome
from buildscripts.quality_checks.models import CandidateFile, ChangeStatus, CheckPhase, CheckSpec

Matcher = Callable[[CandidateFile], bool]
Runner = Callable[[CheckContext, tuple[str, ...]], RunOutcome]


@dataclass(frozen=True, slots=True)
class RegisteredCheck:
    spec: CheckSpec
    matcher: Matcher
    runner: Runner


def _any_file(_candidate: CandidateFile) -> bool:
    return True


def _has_suffix(candidate: CandidateFile, suffixes: tuple[str, ...]) -> bool:
    return any(path.endswith(suffixes) for path in candidate.selection_paths)


def _has_name(candidate: CandidateFile, names: set[str]) -> bool:
    return any(Path(path).name in names for path in candidate.selection_paths)


def _build_file(candidate: CandidateFile) -> bool:
    return any(
        path.endswith((".bazel", ".bzl")) or Path(path).name in {"BUILD", "BUILD.bazel"}
        for path in candidate.selection_paths
    )


def _clang_tidy_file(candidate: CandidateFile) -> bool:
    return _build_file(candidate) or any(
        Path(path).name == ".clang-tidy" for path in candidate.selection_paths
    )


def _target_coverage_file(candidate: CandidateFile) -> bool:
    return _build_file(candidate) or _has_suffix(candidate, (".cpp", ".idl", ".js", ".py"))


def _module_lockfile_input(candidate: CandidateFile) -> bool:
    return _build_file(candidate) or _has_name(candidate, {"MODULE.bazel", "MODULE.bazel.lock"})


def _generated_evergreen_file(candidate: CandidateFile) -> bool:
    return any(
        path == "etc/evergreen.yml"
        or path.startswith("buildscripts/copybara/")
        or path.startswith("etc/evergreen_yml_components/copybara/")
        for path in candidate.selection_paths
    )


def _resmoke_tags_file(candidate: CandidateFile) -> bool:
    return _build_file(candidate) or any(
        path.startswith("etc/evergreen_yml_components/tasks/") and path.endswith(".yml")
        for path in candidate.selection_paths
    )


def _streams_file(candidate: CandidateFile) -> bool:
    return any(
        "jstests/streams" in path or "modules/streams/suites/streams" in path
        for path in candidate.selection_paths
    )


def _codeowners_file(candidate: CandidateFile) -> bool:
    if candidate.status in {
        ChangeStatus.ADDED,
        ChangeStatus.COPIED,
        ChangeStatus.DELETED,
        ChangeStatus.RENAMED,
        ChangeStatus.UNTRACKED,
    }:
        return True
    return any(
        Path(path).name in {"OWNERS.yml", "OWNERS.yaml"}
        or path == ".github/CODEOWNERS"
        or "codeowners" in path.lower()
        for path in candidate.selection_paths
    )


_BAZEL_SERVER_DEPENDENCY = ("bazel.server",)


CHECKS: tuple[RegisteredCheck, ...] = (
    RegisteredCheck(
        CheckSpec(
            "bazel.server",
            "Bazel server startup",
            "bazel",
            CheckPhase.PREFLIGHT_SERIAL,
        ),
        _any_file,
        adapters.run_bazel_server,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.clang-tidy-config",
            "Clang-tidy configuration coverage",
            "lint",
            CheckPhase.MUTATING_SERIAL,
            supports_fix=True,
        ),
        _clang_tidy_file,
        adapters.run_clang_tidy_config,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.bazel-groups",
            "Bazel unittest groups",
            "lint",
            CheckPhase.MUTATING_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
            supports_fix=True,
        ),
        _build_file,
        adapters.run_bazel_groups,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.private-headers",
            "Private header placement",
            "lint",
            CheckPhase.MUTATING_SERIAL,
            supports_fix=True,
        ),
        _build_file,
        adapters.run_private_headers,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.tcmalloc-coverage",
            "Tcmalloc test coverage",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        _build_file,
        adapters.run_tcmalloc_coverage,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.idl-naming",
            "IDL target naming",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        _build_file,
        adapters.run_idl_naming,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.target-coverage",
            "Bazel target coverage",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
            supports_fix=True,
        ),
        _target_coverage_file,
        adapters.run_target_coverage,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.sbom",
            "SBOM validation",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        lambda candidate: _has_name(candidate, {"sbom.private.json"}),
        adapters.run_sbom,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.quickmongolint",
            "Quick Mongo C++ lint",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        lambda candidate: _has_suffix(candidate, (".h", ".cpp")),
        adapters.run_quickmongolint,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.errorcodes",
            "Error-code validation",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        lambda candidate: _has_suffix(candidate, (".cpp", ".c", ".h", ".py", ".idl")),
        adapters.run_errorcodes,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.pyright",
            "Pyright",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        lambda candidate: _has_suffix(candidate, (".py",)),
        adapters.run_pyright,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.module-lockfile",
            "MODULE.bazel.lock",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
            supports_fix=True,
        ),
        _module_lockfile_input,
        adapters.run_module_lockfile,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.uv-lockfile",
            "uv.lock",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        lambda candidate: _has_name(candidate, {"uv.lock", "pyproject.toml"}),
        adapters.run_uv_lockfile,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.generated-evergreen",
            "Generated Evergreen configuration",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
            supports_fix=True,
        ),
        _generated_evergreen_file,
        adapters.run_generated_evergreen,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.copybara-forbidden-text",
            "Copybara forbidden text",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        _any_file,
        adapters.run_copybara_forbidden_text,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.yaml",
            "Evergreen and YAML lint",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        lambda candidate: _has_suffix(candidate, (".yml", ".yaml")),
        adapters.run_yaml,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.resmoke-tags",
            "Evergreen/Bazel resmoke tag parity",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
            supports_fix=True,
        ),
        _resmoke_tags_file,
        adapters.run_resmoke_tags,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.streams-coverage",
            "Streams suite coverage",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        _streams_file,
        adapters.run_streams_coverage,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.markdown-links",
            "Markdown links",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        lambda candidate: _has_suffix(candidate, (".md",)),
        adapters.run_markdown_links,
    ),
    RegisteredCheck(
        CheckSpec("lint.file-size", "Changed-file size", "lint", CheckPhase.SAFE_PARALLEL),
        _any_file,
        adapters.run_file_size,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.module-mapping",
            "Module mapping",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        lambda candidate: _has_suffix(
            candidate, (".cpp", ".c", ".h", ".hpp", ".idl", ".inl", ".defs")
        ),
        adapters.run_module_mapping,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.duplicate-library",
            "Duplicate library names",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
        ),
        _build_file,
        adapters.run_duplicate_library,
    ),
    RegisteredCheck(
        CheckSpec(
            "lint.rules-lint",
            "Combined rules_lint checks",
            "lint",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
            supports_fix=True,
        ),
        lambda candidate: _has_suffix(candidate, (".py", ".js", ".mjs", ".rs")),
        adapters.run_combined_rules_lint,
    ),
    RegisteredCheck(
        CheckSpec(
            "format.formatters",
            "Repository formatters",
            "format",
            CheckPhase.MUTATING_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
            description="Prettier, Aspect formatters, and multiversion backports sorting",
            supports_fix=True,
        ),
        _any_file,
        adapters.run_format,
    ),
    RegisteredCheck(
        CheckSpec(
            "codeowners.local",
            "CODEOWNERS generation and validation",
            "codeowners",
            CheckPhase.MUTATING_SERIAL,
            dependencies=_BAZEL_SERVER_DEPENDENCY,
            supports_fix=True,
        ),
        _codeowners_file,
        adapters.run_codeowners,
    ),
    RegisteredCheck(
        CheckSpec(
            "codeowners.github",
            "GitHub CODEOWNERS validation",
            "codeowners",
            CheckPhase.SHARED_STATE_SERIAL,
            dependencies=("codeowners.local",),
        ),
        _codeowners_file,
        adapters.run_codeowners_github,
    ),
)


def validate_manifest(checks: tuple[RegisteredCheck, ...] = CHECKS) -> None:
    by_id: dict[str, RegisteredCheck] = {}
    for check in checks:
        if check.spec.check_id in by_id:
            raise ValueError(f"duplicate quality check id: {check.spec.check_id}")
        by_id[check.spec.check_id] = check
    for check in checks:
        missing = [dep for dep in check.spec.dependencies if dep not in by_id]
        if missing:
            raise ValueError(f"{check.spec.check_id} has unknown dependencies: {missing}")
    phase_order = {
        CheckPhase.PREFLIGHT_SERIAL: 0,
        CheckPhase.MUTATING_SERIAL: 1,
        CheckPhase.SAFE_PARALLEL: 2,
        CheckPhase.SHARED_STATE_SERIAL: 3,
    }
    for check in checks:
        invalid = [
            dependency
            for dependency in check.spec.dependencies
            if phase_order[by_id[dependency].spec.phase] > phase_order[check.spec.phase]
        ]
        if invalid:
            raise ValueError(
                f"{check.spec.check_id} depends on checks in a later execution phase: {invalid}"
            )
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(check_id: str) -> None:
        if check_id in visiting:
            raise ValueError(f"quality check dependency cycle includes {check_id}")
        if check_id in visited:
            return
        visiting.add(check_id)
        for dependency in by_id[check_id].spec.dependencies:
            visit(dependency)
        visiting.remove(check_id)
        visited.add(check_id)

    for check_id in by_id:
        visit(check_id)
