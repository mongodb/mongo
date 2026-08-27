"""Find the previous release tag relative to a given target commit.

Given a target commit, this module returns the release tag closest in history
to the target, never one that points at or descends from the target. The
target may be on master or on any other branch (including a release branch).

Selection rule:
  Among all candidate tags (matching the configurable ``tag_pattern``, default
  ``r*.*.*``), prefer the one whose merge-base with the target is closest to
  the target (smallest fork distance). Ties are broken by highest version.
  This selects either:
    - the latest tag on the target's own branch (when the target is on a release
      branch with ancestor tags), or
    - the highest-version tag on master / on the most recently forked branch off
      master (when the target is on master or a feature branch off master).

Tags reachable from the target only by going forward in history (i.e. tags whose
commit equals or is descended from the target) are excluded -- the result is
always the *previous* tag, never one that points at the target itself.
"""

from __future__ import annotations

import os
import re
from typing import Optional

import structlog
from git import Commit, Repo

LOGGER = structlog.getLogger(__name__)

DEFAULT_TAG_PATTERN = "r*.*.*"

# A final (GA) release tag: `r8.0.29`, not `r8.0.30-rc0`, `r9.0.0-alpha0` or `r8.0.13-s8-0`.
# `tag_pattern` is a git glob and cannot express "no suffix", so pre-releases are filtered
# out after listing rather than by the pattern.
FINAL_RELEASE_TAG_RE = re.compile(r"^r\d+\.\d+\.\d+$")


def _list_candidate_tags(repo: Repo, pattern: str) -> list[str]:
    """List tags matching ``pattern`` in descending version order.

    Uses ``versionsort.suffix=-`` so the hyphen is treated as a pre-release
    marker -- this places e.g. ``r8.3.1-rc0`` and ``r8.3.1-alpha0`` below
    ``r8.3.1``. Comparison within a suffix class follows git's versioncmp
    (digit runs compared numerically; the rest lexicographic, which happens to
    match semver for alpha/beta/rc).
    """
    stdout = repo.git.execute(
        ["git", "-c", "versionsort.suffix=-", "tag", "-l", pattern, "--sort=-v:refname"],
    )
    return [line for line in stdout.splitlines() if line]


def find_previous_release_tag(
    target_commit_ref: str = "HEAD",
    *,
    repo_root: Optional[str] = None,
    tag_pattern: str = DEFAULT_TAG_PATTERN,
    exclude_prerelease: bool = False,
) -> Optional[str]:
    """Return the previous release tag relative to ``target_commit_ref``.

    "Previous" means the release tag closest in history to the target, never
    one that points at or descends from the target itself.

    ``tag_pattern`` is a git tag glob (the same syntax accepted by
    ``git tag -l``). Use it to narrow the search to a major or major.minor
    series, e.g. ``r9.*.*`` or ``r9.0.*``. Defaults to ``r*.*.*``.

    ``exclude_prerelease`` restricts candidates to final (GA) tags, dropping release
    candidates, alphas and special builds. This is what keeps a series that has
    never released -- master, whose only in-series tag is an alpha -- from
    resolving anything, and stops a newer release candidate from being preferred
    over the last actual release once one is cut.

    Returns ``None`` only when no tags matching ``tag_pattern`` exist in the
    repository. Invalid targets / repository issues are surfaced via the
    underlying GitPython exception types (for example
    :class:`git.exc.GitCommandError`, :class:`gitdb.exc.BadName`,
    :class:`git.exc.InvalidGitRepositoryError`, :class:`git.exc.NoSuchPathError`).
    """
    if repo_root is None:
        # GitPython's repo discovery does not honor GIT_DIR: when GIT_DIR is set
        # (e.g. by a Bazel action running from an output directory with no .git
        # ancestor) but GIT_WORK_TREE is not, it raises InvalidGitRepositoryError
        # even though the plain git CLI resolves GIT_DIR fine. GIT_WORK_TREE points
        # at the working tree root, which GitPython resolves on its own -- including
        # following the .git file of a linked worktree -- so prefer it when present.
        repo_root = os.environ.get("GIT_WORK_TREE")
    repo = Repo(repo_root)
    target_commit = repo.commit(target_commit_ref)
    LOGGER.debug("target commit is valid", target_commit=target_commit)
    tags = _list_candidate_tags(repo, tag_pattern)
    LOGGER.debug("listed candidate tags", count=len(tags), pattern=tag_pattern)
    if exclude_prerelease:
        tags = [tag for tag in tags if FINAL_RELEASE_TAG_RE.match(tag)]
        LOGGER.debug("filtered to final release tags", count=len(tags))
    if not tags:
        return None

    min_distance = -1
    best_tag: Optional[str] = None
    seen_forks: set[Commit] = set()

    for tag in tags:
        # equivalent to: git merge-base --is-ancestor <target_commit> <tag>
        if repo.is_ancestor(target_commit, tag):
            LOGGER.debug("skip tag: target is ancestor of tag", tag=tag)
            continue

        # equivalent to: git merge-base <target_commit> <tag>
        # In criss-cross merges this can return multiple bases; the first one
        # is sufficient for our distance computation.
        merge_base = repo.merge_base(target_commit, tag)
        if not merge_base:
            LOGGER.debug("skip tag: no common ancestor with target", tag=tag)
            continue

        fork_point = merge_base[0]

        fork_distance = int(repo.git.rev_list("--count", f"{fork_point}..{target_commit}"))
        LOGGER.debug("evaluated fork point", tag=tag, fork=fork_point, fork_distance=fork_distance)

        # Symmetric distance is always >= fork distance (target side alone),
        # so once the fork distance for a candidate already meets or exceeds
        # the best symmetric distance seen so far, this candidate cannot
        # improve on it. Remaining candidates are older versions and on real
        # MongoDB release topologies their fork distance only grows from here
        # (master ancestors monotonically; release-branch siblings collapsed
        # via seen_forks). Bail out instead of doing more git work.
        if min_distance != -1 and fork_distance >= min_distance:
            LOGGER.debug(
                "short-circuit: fork distance exceeds best symmetric distance",
                tag=tag,
                fork_distance=fork_distance,
                min_distance=min_distance,
            )
            break

        # Older tags on the exact same branch (pre-release variants) collapse to
        # the highest version already accepted at this fork point.
        if fork_point in seen_forks:
            LOGGER.debug(
                "skip tag: fork point already claimed by higher version",
                tag=tag,
                fork=fork_point,
            )
            continue
        seen_forks.add(fork_point)

        distance = int(repo.git.rev_list("--count", f"{target_commit}...{tag}"))
        LOGGER.debug("computed symmetric distance", tag=tag, distance=distance)

        if min_distance == -1 or distance < min_distance:
            LOGGER.debug(
                "new best tag",
                tag=tag,
                distance=distance,
                previous_best=best_tag,
                previous_distance=min_distance if min_distance != -1 else None,
            )
            min_distance = distance
            best_tag = tag

    LOGGER.debug("selection complete", selected=best_tag, distance=min_distance)
    return best_tag
