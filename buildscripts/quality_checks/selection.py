"""Candidate-file discovery and normalization."""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

from buildscripts.quality_checks.models import CandidateFile, ChangeStatus

MAX_COMMIT_DISTANCE = 100


@dataclass(frozen=True, slots=True)
class DiscoveryResult:
    candidates: tuple[CandidateFile, ...]
    origin_branch: str
    selected_all: bool = False
    warning: str | None = None


def _run_git(repo_root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", *args], cwd=repo_root, check=True, capture_output=True, text=True
    )
    return result.stdout


def resolve_origin_branch(repo_root: Path, origin_branch: str) -> str:
    """Resolve ``auto`` using MongoDB's shared default-branch logic."""

    if origin_branch != "auto":
        return origin_branch
    from git import Repo

    from buildscripts.bazel_rules_mongo.utils.evergreen_git import get_default_origin_branch

    # The shared helper announces the chosen remote on stdout. The unified
    # runner owns progress output, so keep candidate discovery deterministic.
    with contextlib.redirect_stdout(io.StringIO()):
        return get_default_origin_branch(Repo(repo_root))


def _normalize_path(path: str) -> str:
    normalized = Path(os.path.normpath(path)).as_posix()
    if normalized in {"", "."} or Path(normalized).is_absolute():
        raise ValueError(f"candidate path must be repository-relative: {path!r}")
    if normalized == ".." or normalized.startswith("../"):
        raise ValueError(f"candidate path escapes the repository: {path!r}")
    return normalized.removeprefix("./")


def _status(token: str) -> ChangeStatus:
    try:
        return ChangeStatus(token[:1])
    except ValueError:
        return ChangeStatus.MODIFIED


def _parse_name_status(repo_root: Path, output: str) -> list[CandidateFile]:
    fields = output.split("\0")
    if fields and fields[-1] == "":
        fields.pop()
    parsed: list[CandidateFile] = []
    index = 0
    while index < len(fields):
        status = _status(fields[index])
        index += 1
        if status in {ChangeStatus.RENAMED, ChangeStatus.COPIED}:
            if index + 1 >= len(fields):
                raise ValueError("malformed git rename/copy status output")
            old_path = _normalize_path(fields[index])
            new_path = _normalize_path(fields[index + 1])
            index += 2
            parsed.append(
                CandidateFile(new_path, status, old_path, (repo_root / new_path).is_file())
            )
            continue
        if index >= len(fields):
            raise ValueError("malformed git name-status output")
        path = _normalize_path(fields[index])
        index += 1
        parsed.append(
            CandidateFile(
                path,
                status,
                exists=status != ChangeStatus.DELETED and (repo_root / path).is_file(),
            )
        )
    return parsed


def _diff_candidates(repo_root: Path, *args: str) -> list[CandidateFile]:
    output = _run_git(repo_root, "diff", "--name-status", "-z", "--find-renames", *args)
    return _parse_name_status(repo_root, output)


def _merge_candidates(candidates: list[CandidateFile]) -> tuple[CandidateFile, ...]:
    by_path: dict[str, CandidateFile] = {}
    structural_statuses = {
        ChangeStatus.ADDED,
        ChangeStatus.COPIED,
        ChangeStatus.RENAMED,
        ChangeStatus.UNTRACKED,
    }
    for candidate in candidates:
        previous = by_path.get(candidate.path)
        if previous is None:
            by_path[candidate.path] = candidate
            continue
        status = candidate.status
        if candidate.status in {ChangeStatus.MODIFIED, ChangeStatus.TYPE_CHANGED} and (
            previous.status in structural_statuses
        ):
            status = previous.status
        by_path[candidate.path] = CandidateFile(
            candidate.path,
            status,
            candidate.old_path or previous.old_path,
            candidate.exists,
        )
    return tuple(by_path[path] for path in sorted(by_path))


def all_repository_files(repo_root: Path) -> tuple[CandidateFile, ...]:
    output = _run_git(repo_root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
    candidates = []
    for raw_path in output.split("\0"):
        if raw_path:
            path = _normalize_path(raw_path)
            candidates.append(
                CandidateFile(path, ChangeStatus.MODIFIED, exists=(repo_root / path).is_file())
            )
    return _merge_candidates(candidates)


def explicit_files(repo_root: Path, paths: list[str]) -> tuple[CandidateFile, ...]:
    candidates = []
    for raw_path in paths:
        path = _normalize_path(raw_path)
        exists = (repo_root / path).is_file()
        candidates.append(
            CandidateFile(
                path,
                ChangeStatus.MODIFIED if exists else ChangeStatus.DELETED,
                exists=exists,
            )
        )
    return _merge_candidates(candidates)


def discover_changed_files(
    repo_root: Path, *, origin_branch: str = "auto", force_all: bool = False
) -> DiscoveryResult:
    """Return branch, index, worktree, and untracked candidates exactly once."""

    if force_all:
        selected_origin = "HEAD" if origin_branch == "auto" else origin_branch
        return DiscoveryResult(all_repository_files(repo_root), selected_origin, selected_all=True)
    resolved_origin = resolve_origin_branch(repo_root, origin_branch)
    distance = int(_run_git(repo_root, "rev-list", "--count", f"{resolved_origin}..HEAD").strip())
    if distance > MAX_COMMIT_DISTANCE:
        warning = (
            f"The branch is {distance} commits from {resolved_origin}, above the "
            f"{MAX_COMMIT_DISTANCE}-commit selection limit; running all applicable checks."
        )
        return DiscoveryResult(
            all_repository_files(repo_root), resolved_origin, selected_all=True, warning=warning
        )
    merge_base = _run_git(repo_root, "merge-base", "HEAD", resolved_origin).strip()
    candidates: list[CandidateFile] = []
    candidates.extend(_diff_candidates(repo_root, f"{merge_base}..HEAD"))
    candidates.extend(_diff_candidates(repo_root, "--cached"))
    candidates.extend(_diff_candidates(repo_root))
    untracked = _run_git(repo_root, "ls-files", "-z", "--others", "--exclude-standard")
    for raw_path in untracked.split("\0"):
        if raw_path:
            path = _normalize_path(raw_path)
            candidates.append(
                CandidateFile(path, ChangeStatus.UNTRACKED, exists=(repo_root / path).is_file())
            )
    return DiscoveryResult(_merge_candidates(candidates), resolved_origin)
