"""Tests for validate_pr_approval_recency gate logic.

Mocks PyGithub objects directly; no HTTP interception needed.

Fixture SHA values:
    HEAD_SHA     = "a" * 40  — the PR's current head commit
    APPROVAL_SHA = "b" * 40  — the commit at which the last approval was submitted
"""

import datetime
import os
import shlex
import shutil
import sys
import tempfile
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

sys.path.insert(0, str(Path(__file__).parents[2]))
from validate_pr_approval_recency import (
    REASON_API_ERROR,
    REASON_CONTENT_CHANGED,
    REASON_CONTENT_UNCHANGED,
    REASON_FILE_LIMIT,
    REASON_PENDING_REVIEW,
    REASON_NO_APPROVALS,
    REASON_SHA_MATCH,
    _check_approved_pr,
    _run_gate,
    main,
)

ORG = "10gen"
REPO = "mongo"
PR_NUMBER = 42
BASE_REF = "main"
HEAD_SHA = "a" * 40
APPROVAL_SHA = "b" * 40


# ---------------------------------------------------------------------------
# Mock factories
# ---------------------------------------------------------------------------

_SUBMITTED_AT = datetime.datetime(2024, 1, 15, 8, 0, tzinfo=datetime.timezone.utc)
_CREATED_AT = datetime.datetime(2024, 1, 15, 9, 0, tzinfo=datetime.timezone.utc)
_UPDATED_AT = datetime.datetime(2024, 1, 15, 10, 0, tzinfo=datetime.timezone.utc)


def _make_review(
    commit_id: str,
    state: str = "APPROVED",
    submitted_at: datetime.datetime = _SUBMITTED_AT,
    review_id: int = 1,
) -> MagicMock:
    r = MagicMock()
    r.state = state
    r.submitted_at = submitted_at
    r.id = review_id
    r.commit_id = commit_id
    return r


def _make_file(
    filename: str, patch: str | None = "@@ -1 +1 @@\n-old\n+new", sha: str = "abc123"
) -> MagicMock:
    f = MagicMock()
    f.filename = filename
    f.patch = patch
    f.sha = sha
    return f


def _make_pull(
    approval_sha: str = APPROVAL_SHA,
    head_sha: str = HEAD_SHA,
    base_ref: str = BASE_REF,
    extra_reviews: list | None = None,
) -> MagicMock:
    pull = MagicMock()
    pull.head.sha = head_sha
    pull.head.ref = "feature-branch"
    pull.base.ref = base_ref
    pull.title = "Test PR"
    pull.user.login = "author"
    pull.state = "open"
    pull.created_at = _CREATED_AT
    pull.updated_at = _UPDATED_AT
    pull.get_reviews.return_value = [_make_review(approval_sha)] + (extra_reviews or [])
    return pull


def _make_repo(approved_files: list | None = None, head_files: list | None = None) -> MagicMock:
    """Repo whose compare() returns different file lists for APPROVAL_SHA vs HEAD_SHA."""
    _approved = approved_files if approved_files is not None else []
    _head = head_files if head_files is not None else []

    def _compare(_: str, sha: str) -> MagicMock:
        cmp = MagicMock()
        cmp.files = _approved if sha == APPROVAL_SHA else _head
        return cmp

    repo = MagicMock()
    repo.compare.side_effect = _compare
    return repo


def _make_client(review_decision: str | None, repo: MagicMock | None = None) -> MagicMock:
    """Github client whose GraphQL query returns the given reviewDecision."""
    client = MagicMock()
    client.requester.graphql_query.return_value = (
        None,
        {"data": {"repository": {"pullRequest": {"reviewDecision": review_decision}}}},
    )
    if repo is not None:
        client.get_repo.return_value = repo
    return client


# ---------------------------------------------------------------------------
# Base class for tests that drive main() (needs env vars + buildevents file)
# ---------------------------------------------------------------------------


class _MainTestBase:
    def setup_method(self) -> None:
        self._tmpdir = tempfile.mkdtemp()
        self.buildevent_path = os.path.join(self._tmpdir, "buildevents.txt")
        os.environ.update(
            {
                "PR_NUMBER": str(PR_NUMBER),
                "GITHUB_ORG": ORG,
                "GITHUB_REPO": REPO,
                "GITHUB_TOKEN": "ghs_test_token_not_real",
                "BUILDEVENT_FILE": self.buildevent_path,
            }
        )

    def teardown_method(self) -> None:
        for key in ("PR_NUMBER", "GITHUB_ORG", "GITHUB_REPO", "GITHUB_TOKEN", "BUILDEVENT_FILE"):
            os.environ.pop(key, None)
        shutil.rmtree(self._tmpdir, ignore_errors=True)

    def read_metrics(self) -> dict:
        text = Path(self.buildevent_path).read_text().strip()
        fields = {}
        for token in shlex.split(text):
            key, _, value = token.partition("=")
            fields[key] = value
        return fields


# ---------------------------------------------------------------------------
# Test classes
# ---------------------------------------------------------------------------


class TestNotApproved:
    """PRs that aren't fully approved are a no-op — branch protection handles them."""

    def test_review_required_passes(self) -> None:
        result = _run_gate(_make_client("REVIEW_REQUIRED"), ORG, REPO, PR_NUMBER, {})
        assert result == REASON_PENDING_REVIEW

    def test_changes_requested_passes(self) -> None:
        result = _run_gate(_make_client("CHANGES_REQUESTED"), ORG, REPO, PR_NUMBER, {})
        assert result == REASON_PENDING_REVIEW

    def test_null_decision_passes(self) -> None:
        result = _run_gate(_make_client(None), ORG, REPO, PR_NUMBER, {})
        assert result == REASON_PENDING_REVIEW


class TestApprovedShaMatch:
    """approval_sha == head_sha → trivial pass; compare API is never called."""

    def test_sha_match_passes_without_compare(self) -> None:
        pull = _make_pull(approval_sha=HEAD_SHA, head_sha=HEAD_SHA)
        repo = MagicMock()
        assert _check_approved_pr(repo, pull, {}) == REASON_SHA_MATCH
        repo.compare.assert_not_called()


class TestApprovedNoReviews:
    """reviewDecision is APPROVED but no APPROVED review found → fail closed."""

    def test_no_reviews_fails_closed(self) -> None:
        pull = MagicMock()
        pull.head.sha = HEAD_SHA
        pull.base.ref = BASE_REF
        pull.get_reviews.return_value = []
        assert _check_approved_pr(MagicMock(), pull, {}) == REASON_NO_APPROVALS


class TestApprovedSameContent:
    """Approved + no-op Graphite restack → introduced content identical → pass."""

    def test_noop_restack_passes(self) -> None:
        f = _make_file("src/foo.cpp", patch="-old\n+new")
        pull = _make_pull()
        repo = _make_repo(approved_files=[f], head_files=[f])
        metrics: dict = {}
        assert _check_approved_pr(repo, pull, metrics) == REASON_CONTENT_UNCHANGED
        assert metrics["was_restack"] is True

    def test_binary_file_unchanged_passes(self) -> None:
        """Binary files with the same blob SHA are treated as unchanged."""
        f = _make_file("img.png", patch=None, sha="1" * 40)
        pull = _make_pull()
        repo = _make_repo(approved_files=[f], head_files=[f])
        assert _check_approved_pr(repo, pull, {}) == REASON_CONTENT_UNCHANGED


class TestApprovedContentChanged:
    """Approved + new code pushed after approval → fail."""

    def test_changed_content_blocks(self) -> None:
        approved_file = _make_file("src/foo.cpp", patch="-old\n+new")
        head_file = _make_file("src/foo.cpp", patch="-old\n+DIFFERENT")
        pull = _make_pull()
        repo = _make_repo(approved_files=[approved_file], head_files=[head_file])
        metrics: dict = {}
        assert _check_approved_pr(repo, pull, metrics) == REASON_CONTENT_CHANGED
        assert metrics["was_restack"] is False
        assert "post_approval_changed_files" in metrics
        assert "post_approval_changed_loc" in metrics
        assert int(metrics["post_approval_changed_files"]) >= 1

    def test_binary_file_changed_blocks(self) -> None:
        """A changed binary (different blob SHA) is detected and blocks merge."""
        approved_file = _make_file("img.png", patch=None, sha="1" * 40)
        head_file = _make_file("img.png", patch=None, sha="2" * 40)
        pull = _make_pull()
        repo = _make_repo(approved_files=[approved_file], head_files=[head_file])
        assert _check_approved_pr(repo, pull, {}) == REASON_CONTENT_CHANGED


class TestApprovedOver300Files:
    """Compare returns ≥ 300 files → can't fully verify the diff → fail closed."""

    def test_300_files_fails_closed(self) -> None:
        files_300 = [_make_file(f"src/file_{i:04d}.cpp") for i in range(300)]
        pull = _make_pull()
        repo = _make_repo(approved_files=files_300, head_files=[])
        assert _check_approved_pr(repo, pull, {}) == REASON_FILE_LIMIT


class TestApiErrors(_MainTestBase):
    """GitHub API failure → fail closed."""

    def test_graphql_error_fails_closed(self) -> None:
        with patch(
            "validate_pr_approval_recency._run_gate", side_effect=RuntimeError("graphql error")
        ):
            assert main() == 1
        assert self.read_metrics()["reason"] == REASON_API_ERROR

    def test_run_gate_raises_on_graphql_errors(self) -> None:
        client = MagicMock()
        client.requester.graphql_query.return_value = (None, {"errors": [{"message": "Not found"}]})
        with pytest.raises(RuntimeError):
            _run_gate(client, ORG, REPO, PR_NUMBER, {})


class TestReviewLifecycle:
    """Three-step story that mirrors a real Graphite PR workflow:

    1. Reviewer approves at commit B.
    2. Author pushes new code (head moves to A, B ≠ A) → gate blocks.
    3. Reviewer re-approves at commit A → gate passes (most recent approval wins).
    4. Author restacks with Graphite (new SHA, same introduced content) → gate passes.
    """

    def test_step1_commit_after_approval_blocks(self) -> None:
        """New code landed after the only approval → fail."""
        approved_file = _make_file("src/foo.cpp", patch="-old\n+approved")
        head_file = _make_file("src/foo.cpp", patch="-old\n+changed")
        pull = _make_pull()
        repo = _make_repo(approved_files=[approved_file], head_files=[head_file])
        metrics: dict = {}
        assert _check_approved_pr(repo, pull, metrics) == REASON_CONTENT_CHANGED
        assert metrics["pr_base_branch"] == BASE_REF
        assert metrics["head_sha"] == HEAD_SHA
        assert metrics["pr_author"] == "author"
        assert metrics["pr_created_at"] == _CREATED_AT.isoformat()

    def test_step2_re_approval_at_head_passes(self) -> None:
        """Reviewer approves again at the new head commit → most recent approval wins → pass.

        Two APPROVED reviews; _latest_approval_commit picks the newer one (commit_id == HEAD_SHA),
        which short-circuits to REASON_SHA_MATCH without calling compare.
        """
        older_approval = _make_review(APPROVAL_SHA, submitted_at=_SUBMITTED_AT, review_id=1)
        newer_approval = _make_review(
            HEAD_SHA,
            submitted_at=_SUBMITTED_AT + datetime.timedelta(hours=1),
            review_id=2,
        )
        pull = _make_pull(approval_sha=APPROVAL_SHA, head_sha=HEAD_SHA)
        pull.get_reviews.return_value = [older_approval, newer_approval]
        repo = MagicMock()
        assert _check_approved_pr(repo, pull, {}) == REASON_SHA_MATCH
        repo.compare.assert_not_called()

    def test_step3_graphite_restack_passes(self) -> None:
        """After re-approval the author restacks; same introduced patch → gate passes."""
        f = _make_file("src/foo.cpp", patch="-old\n+new")
        pull = _make_pull()
        repo = _make_repo(approved_files=[f], head_files=[f])
        assert _check_approved_pr(repo, pull, {}) == REASON_CONTENT_UNCHANGED


class TestMissingConfig(_MainTestBase):
    """Missing required env vars → fail closed with api_error."""

    def test_missing_pr_number_fails_closed(self) -> None:
        os.environ.pop("PR_NUMBER", None)
        assert main() == 1
        metrics = self.read_metrics()
        assert metrics["reason"] == REASON_API_ERROR
        assert metrics["result"] == "error"
