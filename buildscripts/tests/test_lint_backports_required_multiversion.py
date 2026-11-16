"""Unit tests for lint_backports_required_multiversion.py.

Two table-driven tests:

* test_check_file: the file's contents, as YAML, and the errors check_file reports for them.
* test_cli: git operations that build a history, and the CLI's exit code and output for it.
"""

import re
import shlex
import subprocess
import tempfile
import textwrap
import unittest
import unittest.mock
from dataclasses import dataclass
from pathlib import Path

import yaml
from typer.testing import CliRunner

from buildscripts import backports_required_multiversion as model
from buildscripts import lint_backports_required_multiversion as lint

TEST_FILE = "jstests/replsets/server_status_metrics.js"
OTHER_TEST_FILE = "jstests/core/query/bson_size_limit_agg.js"
SUITE_KEY = "replica_sets_multiversion"


@dataclass(frozen=True)
class FileCase:
    contents: str
    # A substring of each error check_file should report, in order.
    errors: tuple[str, ...] = ()


FILE_CASES = {
    "invalid YAML": FileCase("last-lts: [", ("Not valid YAML",)),
    "every section": FileCase(
        f"""
        last-lts:
          all:
          suites:
        last-continuous:
          all:
          - test_file: {TEST_FILE}
            ticket: SERVER-1
          suites:
            {SUITE_KEY}:
            - test_file: {OTHER_TEST_FILE}
              ticket: SERVER-2
        last-patch:
          all: []
          suites: {{}}
        """
    ),
    "top level is a list": FileCase(
        """
        - last-lts
        """,
        ("The top level must be a mapping",),
    ),
    "unknown section": FileCase(
        """
        last_lts:
          all:
          suites:
        """,
        ("Unknown section 'last_lts'",),
    ),
    "section missing suites": FileCase(
        """
        last-lts:
          all:
        """,
        ("last-lts: missing key 'suites'",),
    ),
    "unknown section key": FileCase(
        """
        last-lts:
          all:
          suites:
          al:
        """,
        ("last-lts: Unknown key 'al'",),
    ),
    "all is a mapping": FileCase(
        f"""
        last-lts:
          all:
            test_file: {TEST_FILE}
            ticket: SERVER-1
          suites:
        """,
        ("last-lts.all",),
    ),
    "entry unknown key": FileCase(
        f"""
        last-lts:
          all:
          - test_file: {TEST_FILE}
            ticket: SERVER-1
            tikcet: SERVER-2
          suites:
        """,
        ("last-lts.all[0]: Unknown key 'tikcet'",),
    ),
    "entry missing ticket": FileCase(
        f"""
        last-lts:
          all:
          - test_file: {TEST_FILE}
          suites:
        """,
        ("last-lts.all[0]: missing key 'ticket'",),
    ),
    **{
        f"malformed ticket {ticket}": FileCase(
            f"""
            last-lts:
              all:
              - test_file: {TEST_FILE}
                ticket: {ticket}
              suites:
            """,
            (f"ticket '{ticket}' is not a Jira key",),
        )
        for ticket in ("SERVER_89698", "SERVER-SERVER-131283", "server-1", "SERVER-0", "SERVER-")
    },
    "ticket is a number": FileCase(
        f"""
        last-lts:
          all:
          - test_file: {TEST_FILE}
            ticket: 1234
          suites:
        """,
        ("last-lts.all[0].ticket",),
    ),
    **{
        f"ticket from project {ticket}": FileCase(
            f"""
            last-lts:
              all:
              - test_file: {TEST_FILE}
                ticket: {ticket}
              suites:
            """
        )
        for ticket in ("WT-12345", "SECBUG-9")
    },
    "duplicate entry": FileCase(
        f"""
        last-lts:
          all:
          - test_file: {TEST_FILE}
            ticket: SERVER-1
          - test_file: {TEST_FILE}
            ticket: SERVER-1
          suites:
        """,
        ("listed more than once",),
    ),
    "same entry in different sections": FileCase(
        f"""
        last-lts:
          all:
          - test_file: {TEST_FILE}
            ticket: SERVER-1
          suites:
        last-continuous:
          all:
          - test_file: {TEST_FILE}
            ticket: SERVER-1
          suites:
        """
    ),
    "suite entries are checked": FileCase(
        f"""
        last-lts:
          all:
          suites:
            {SUITE_KEY}:
            - test_file: {TEST_FILE}
              ticket: X
        """,
        ("ticket 'X' is not a Jira key",),
    ),
    "model errors are reported together": FileCase(
        f"""
        last-lts:
          all:
          - test_file: {TEST_FILE}
            ticket: SERVER_1
          suites:
        last-continuous:
          all:
          - test_file: {TEST_FILE}
          suites:
        """,
        ("SERVER_1", "missing key 'ticket'"),
    ),
}


@dataclass(frozen=True)
class Commit:
    message: str
    # The backports file's new contents, as YAML. None commits without changing it.
    contents: str | None = None


def publish(branch: str) -> str:
    """Point `origin/<branch>` at HEAD, as if HEAD had been pushed."""
    return f"update-ref refs/remotes/origin/{branch} HEAD"


EMPTY = """
last-continuous:
  all:
  suites:
"""
METRIC = f"""
last-continuous:
  all:
  - test_file: {TEST_FILE}
    ticket: SERVER-120318
  suites:
"""

# Master gains SERVER-120318 and its denylist entry after v1.0 is cut, as in BF-44237.
RELEASE_CUT = (
    Commit("SERVER-10 initial", EMPTY),
    "branch v1.0",
    Commit("SERVER-120318 Add metric", METRIC),
    publish("master"),
    "checkout -q v1.0",
    Commit("SERVER-11 [v1.0] Unrelated release change"),
    publish("v1.0"),
)
BAD_BACKPORT = (*RELEASE_CUT, Commit("SERVER-113184 [v1.0] Backport initial sync fix", METRIC))
NOT_MENTIONED = "no commit on this branch mentions SERVER-120318"
PASSES = "No problems found"


@dataclass(frozen=True)
class CliCase:
    # Each operation is a Commit or the arguments to a git command.
    ops: tuple[Commit | str, ...]
    args: tuple[str, ...] = ()
    # Written to expansions.yml and passed as --expansions-file.
    expansions: dict[str, str] | None = None
    default_origin_branch: str = "v1.0"
    exit_code: int = 0
    # Substrings of the output. `<rev>` is replaced by `git rev-parse rev` after the ops run.
    output: tuple[str, ...] = ()
    not_output: tuple[str, ...] = ()


CLI_CASES = {
    "cherry-picked entry without its fix": CliCase(
        BAD_BACKPORT,
        exit_code=1,
        output=(f"last-continuous.all: {TEST_FILE} (SERVER-120318): {NOT_MENTIONED}.",),
    ),
    "entry for a backported ticket": CliCase(
        (*RELEASE_CUT, Commit("SERVER-120318 [v1.0] Add metric", METRIC)), output=(PASSES,)
    ),
    "ticket mentioned in the commit body": CliCase(
        (
            *RELEASE_CUT,
            Commit("SERVER-133682 [v1.0] Record shutdowns\n\nPart of SERVER-120318.", METRIC),
        ),
        output=(PASSES,),
    ),
    "ticket backported by an earlier commit": CliCase(
        (
            *RELEASE_CUT,
            Commit("SERVER-120318 [v1.0] Add metric"),
            publish("v1.0"),
            Commit("SERVER-13 [v1.0] Unrelated", METRIC),
        ),
        output=(PASSES,),
    ),
    "ticket prefix does not match": CliCase(
        (*RELEASE_CUT, Commit("SERVER-1203189 [v1.0] Similar ticket number", METRIC)),
        exit_code=1,
        output=(NOT_MENTIONED,),
    ),
    "existing entries are not reported": CliCase(
        (
            *RELEASE_CUT,
            Commit("SERVER-14 [v1.0] Earlier mistake", METRIC),
            publish("v1.0"),
            Commit("SERVER-15 [v1.0] Unrelated"),
        ),
        output=(PASSES,),
    ),
    "moving an entry between sections counts as adding it": CliCase(
        (
            *RELEASE_CUT,
            Commit("SERVER-14 [v1.0] Earlier mistake", METRIC),
            publish("v1.0"),
            Commit(
                "SERVER-16 [v1.0] Move entry",
                f"""
                last-lts:
                  all:
                  - test_file: {TEST_FILE}
                    ticket: SERVER-120318
                  suites:
                """,
            ),
        ),
        exit_code=1,
        output=(f"last-lts.all: {TEST_FILE} (SERVER-120318): {NOT_MENTIONED}",),
    ),
    "suite-scoped entries are checked": CliCase(
        (
            *RELEASE_CUT,
            Commit(
                "SERVER-17 [v1.0] Suite-scoped entry",
                f"""
                last-continuous:
                  all:
                  suites:
                    {SUITE_KEY}:
                    - test_file: {TEST_FILE}
                      ticket: SERVER-120318
                """,
            ),
        ),
        exit_code=1,
        output=(f"last-continuous.suites.{SUITE_KEY}: {TEST_FILE} (SERVER-120318)",),
    ),
    "every entry is added when the base lacks the file": CliCase(
        (
            *RELEASE_CUT,
            f"rm -q {model.BACKPORTS_FILE}",
            Commit("SERVER-20 [v1.0] Remove the file"),
            publish("v1.0"),
            Commit("SERVER-21 [v1.0] Restore the file", METRIC),
        ),
        exit_code=1,
        output=(NOT_MENTIONED,),
    ),
    "nothing is compared when the base does not parse": CliCase(
        # The base may predate a rule the model enforces, like the ticket format.
        (
            *RELEASE_CUT,
            Commit(
                "SERVER-18 [v1.0] Malformed ticket",
                f"""
                last-continuous:
                  all:
                  - test_file: {TEST_FILE}
                    ticket: SERVER_1
                  suites:
                """,
            ),
            publish("v1.0"),
            Commit(
                "SERVER-19 [v1.0] Fix the ticket",
                f"""
                last-continuous:
                  all:
                  - test_file: {TEST_FILE}
                    ticket: SERVER-1
                  - test_file: {OTHER_TEST_FILE}
                    ticket: SERVER-120318
                  suites:
                """,
            ),
        ),
        output=(PASSES,),
    ),
    "auto compares against the merge base with DEFAULT_ORIGIN_BRANCH": CliCase(
        (*RELEASE_CUT, Commit("SERVER-12 [v1.0] Unrelated")),
        output=("Checked entries added relative to <origin/v1.0>.", PASSES),
    ),
    "explicit origin branch": CliCase(
        BAD_BACKPORT,
        args=("--origin-branch", "origin/v1.0"),
        exit_code=1,
        output=("Checked entries added relative to origin/v1.0.", NOT_MENTIONED),
    ),
    "nothing is compared when the origin branch is HEAD": CliCase(
        BAD_BACKPORT, args=("--origin-branch", "HEAD"), output=(PASSES,)
    ),
    "missing explicit origin branch": CliCase(
        (*BAD_BACKPORT, "update-ref -d refs/remotes/origin/v1.0"),
        args=("--origin-branch", "origin/v1.0"),
        exit_code=1,
        output=("Could not find the merge base of HEAD and origin/v1.0",),
    ),
    "missing DEFAULT_ORIGIN_BRANCH": CliCase(
        (*BAD_BACKPORT, "update-ref -d refs/remotes/origin/v1.0"),
        exit_code=1,
        output=("Could not find the base commit",),
    ),
    "master is not checked": CliCase(
        # Denylisting ahead of time with a ticket that has not landed anywhere is legitimate on
        # master: entries there can only ever exclude more tests.
        BAD_BACKPORT,
        default_origin_branch="master",
        output=(PASSES,),
        not_output=("Checked entries added",),
    ),
    "merge queue compares against the parent commit": CliCase(
        BAD_BACKPORT,
        expansions={"project": "mongodb-mongo"},
        exit_code=1,
        output=("Checked entries added relative to <HEAD^1>.", NOT_MENTIONED),
    ),
    "GitHub PR compares against the merge base with its base branch": CliCase(
        (
            *BAD_BACKPORT,
            "checkout -q -b pr",
            "branch -f v1.0 origin/v1.0",
            Commit("SERVER-12 [v1.0] Another commit in the PR"),
        ),
        expansions={"is_patch": "true", "github_pr_number": "1", "branch_name": "v1.0"},
        exit_code=1,
        output=("Checked entries added relative to <v1.0>.", NOT_MENTIONED),
    ),
    "GitHub PR without a local base branch": CliCase(
        BAD_BACKPORT,
        expansions={"is_patch": "true", "github_pr_number": "1", "branch_name": "v9.9"},
        exit_code=1,
        output=("Could not find the base commit",),
    ),
}


def _git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", *args], cwd=root, check=True, capture_output=True, text=True
    ).stdout.strip()


def _run_ops(root: Path, ops: tuple[Commit | str, ...]) -> None:
    _git(root, "init", "-q", "-b", "master")
    _git(root, "config", "user.email", "test@example.com")
    _git(root, "config", "user.name", "Test")
    _git(root, "config", "commit.gpgsign", "false")
    _git(root, "remote", "add", "origin", "https://github.com/mongodb/mongo.git")
    for op in ops:
        if isinstance(op, str):
            _git(root, *shlex.split(op))
            continue
        if op.contents is not None:
            path = root / model.BACKPORTS_FILE
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(textwrap.dedent(op.contents))
            _git(root, "add", model.BACKPORTS_FILE)
        _git(root, "commit", "-q", "--allow-empty", "-m", op.message)


class TestLintBackportsRequiredMultiversion(unittest.TestCase):
    def test_check_file(self):
        for name, case in FILE_CASES.items():
            with self.subTest(name):
                errors = lint.check_file(textwrap.dedent(case.contents))
                self.assertEqual(len(errors), len(case.errors), errors)
                for expected, error in zip(case.errors, errors):
                    self.assertIn(expected, error)

    def test_cli(self):
        for name, case in CLI_CASES.items():
            with (
                self.subTest(name),
                tempfile.TemporaryDirectory() as tmp,
                unittest.mock.patch.object(
                    lint.evergreen_git, "DEFAULT_ORIGIN_BRANCH", case.default_origin_branch
                ),
            ):
                # get_diff_revision caches by argument, which would leak one case's repo into
                # the next.
                lint.evergreen_git.get_diff_revision.cache_clear()
                root = Path(tmp)
                _run_ops(root, case.ops)
                args = list(case.args)
                if case.expansions is not None:
                    (root / "expansions.yml").write_text(yaml.safe_dump(case.expansions))
                    args += ["--expansions-file", "expansions.yml"]

                # `bazel run` points the script at the checkout through this variable.
                result = CliRunner().invoke(
                    lint.app, args, env={"BUILD_WORKSPACE_DIRECTORY": str(root)}
                )

                self.assertEqual(result.exit_code, case.exit_code, result.output)
                for expected in case.output:
                    expected = re.sub(r"<(.+?)>", lambda m: _git(root, "rev-parse", m[1]), expected)
                    self.assertIn(expected, result.output)
                for unexpected in case.not_output:
                    self.assertNotIn(unexpected, result.output)
        lint.evergreen_git.get_diff_revision.cache_clear()


if __name__ == "__main__":
    unittest.main()
