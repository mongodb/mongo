#!/usr/bin/env python3
"""
This linter checks that backports_required_for_multiversion_tests.yml is well-formed.
When run from a release branch, it also checks that any newly added entries are consistent
with the tickets that have actually been backported to that branch,

See also etc/backports_required_for_multiversion_tests.yml.
"""

import contextlib
import io
import os
import re
import subprocess
from pathlib import Path
from typing import Annotated

import typer
from git import GitCommandError

from buildscripts.backports_required_multiversion import (
    BACKPORTS_FILE,
    BackportsRequired,
    InvalidBackportsFile,
    parse_text,
)

try:
    from buildscripts.bazel_rules_mongo.utils import evergreen_git
except ModuleNotFoundError:
    # Under Bazel, bazel_rules_mongo is a separate module whose root is on the import path.
    from utils import evergreen_git

TICKET_MENTION_RE = re.compile(r"(?<![A-Za-z0-9_-])[A-Z][A-Z0-9]*-[1-9][0-9]*(?![0-9])")
MASTER = "master"

ADDED_ENTRIES_HELP = """\
An entry in this file tells newer branches that the ticket's fix is on this branch, so they stop
excluding the test from multiversion suites that run against this branch's binaries. No commit
on this branch mentions the ticket, so the fix is probably not here. This usually comes from
resolving a merge conflict in this file during a backport by keeping the other side's entries.

To fix it, do one of the following:
  * Remove the entry, unless this change backports that ticket's fix.
  * If the fix is on this branch under another key (for example, the ticket is the parent of
    backported sub-tasks), mention the ticket in your commit message, for example "Part of
    SERVER-12345".
"""


def check_file(text: str) -> list[str]:
    """Return a message for every problem in the file's contents on this branch."""
    try:
        parse_text(text)
    except InvalidBackportsFile as err:
        return err.errors
    return []


def _git(repo_root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args], cwd=repo_root, capture_output=True, text=True, check=check
    )


class UnparsableRevision(Exception):
    """The file at a revision is not valid YAML or does not match the model's structure."""


def _load_file_at(repo_root: Path, revision: str) -> BackportsRequired:
    """Parse the file at `revision`, validating its structure."""
    result = _git(repo_root, "show", f"{revision}:{BACKPORTS_FILE}", check=False)
    if result.returncode != 0:
        # The file does not exist at this revision.
        return BackportsRequired({})
    try:
        return parse_text(result.stdout)
    except InvalidBackportsFile as err:
        raise UnparsableRevision(revision) from err


class OriginBranchNotFound(Exception):
    """The base to compare against could not be resolved, usually because a ref was not fetched."""


def is_release_branch() -> bool:
    return evergreen_git.DEFAULT_ORIGIN_BRANCH != MASTER


def resolve_base(repo_root: Path, origin_branch: str, expansions_file: Path | None) -> str:
    """Return the revision that the change's added entries are measured from."""
    if origin_branch != "auto":
        return origin_branch
    try:
        # get_diff_revision reads the repository in the working directory and prints the base.
        with contextlib.chdir(repo_root), contextlib.redirect_stdout(io.StringIO()):
            return evergreen_git.get_diff_revision(
                str(repo_root / expansions_file) if expansions_file else None,
                evergreen_git.DEFAULT_ORIGIN_BRANCH,
            )
    except (AssertionError, IndexError, GitCommandError, RuntimeError) as err:
        raise OriginBranchNotFound(
            f"Could not find the base commit to compare against: {err!r}"
        ) from err


def check_added_entries(repo_root: Path, origin_branch: str) -> list[str]:
    """Report entries added since `origin_branch` whose ticket no commit on the branch mentions.

    Reports nothing when there is nothing to compare: `origin_branch` is HEAD, or the file at
    either end does not parse.
    """
    merge_base = _git(repo_root, "merge-base", "HEAD", origin_branch, check=False)
    if merge_base.returncode != 0:
        raise OriginBranchNotFound(
            f"Could not find the merge base of HEAD and {origin_branch}: "
            f"{merge_base.stderr.strip() or 'no common ancestor'}. Fetch it, or pass "
            "--origin-branch to select a different origin branch."
        )
    base = merge_base.stdout.strip()
    if base == _git(repo_root, "rev-parse", "HEAD").stdout.strip():
        return []
    try:
        head = _load_file_at(repo_root, "HEAD")
        base_lists = {
            where: set(entries) for where, entries in _load_file_at(repo_root, base).lists()
        }
    except UnparsableRevision:
        return []

    messages = _git(repo_root, "log", "--format=%B", "HEAD").stdout
    mentioned = set(TICKET_MENTION_RE.findall(messages))
    return [
        f"{where}: {entry}: no commit on this branch mentions {entry.ticket}."
        for where, entries in head.lists()
        for entry in entries
        if entry not in base_lists.get(where, set()) and entry.ticket not in mentioned
    ]


def _repo_root() -> Path:
    """Return the checkout to lint: the one `bazel run` was invoked from, else this script's."""
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    return Path(workspace).resolve() if workspace else Path(__file__).resolve().parents[1]


def main(
    origin_branch: Annotated[
        str,
        typer.Option(
            help="Base branch to compare changes against (example: origin/<branch>). `auto` "
            "uses the CI base from --expansions-file, else the branch's DEFAULT_ORIGIN_BRANCH."
        ),
    ] = "auto",
    expansions_file: Annotated[
        Path | None,
        typer.Option(help="Evergreen expansions file, relative to the repository root, in CI."),
    ] = None,
) -> None:
    repo_root = _repo_root()
    text = (repo_root / BACKPORTS_FILE).read_text(encoding="utf-8")
    file_errors = check_file(text)

    added_entry_errors = []
    if is_release_branch():
        try:
            base = resolve_base(repo_root, origin_branch, expansions_file)
            added_entry_errors = check_added_entries(repo_root, base)
        except OriginBranchNotFound as err:
            typer.echo(err)
            raise typer.Exit(code=1)
        typer.echo(f"Checked entries added relative to {base}.")

    if file_errors:
        typer.echo(f"\n{BACKPORTS_FILE} is malformed:")
        typer.echo("\n".join(f"  {error}" for error in file_errors))
    if added_entry_errors:
        typer.echo(
            f"\n{BACKPORTS_FILE} adds entries whose fix does not appear to be on this branch:"
        )
        typer.echo("\n".join(f"  {error}" for error in added_entry_errors))
        typer.echo("\n" + ADDED_ENTRIES_HELP)
    if file_errors or added_entry_errors:
        raise typer.Exit(code=1)
    typer.echo(f"No problems found in {BACKPORTS_FILE}.")


app = typer.Typer(
    pretty_exceptions_show_locals=False, add_completion=False, rich_markup_mode="markdown"
)
app.command(help=__doc__)(main)

if __name__ == "__main__":
    app()
