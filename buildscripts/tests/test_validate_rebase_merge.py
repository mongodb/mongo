"""Unit tests for the validate_rebase_merge script."""

import unittest
from unittest.mock import patch

import typer
from git import Commit, Repo

from buildscripts.validate_rebase_merge import (
    check_banned_strings,
    check_feature_branch_tickets,
    feature_branch_tickets,
    get_pr_commits,
)


def make_commit(message: str) -> Commit:
    fake_repo = Repo()
    return Commit(repo=fake_repo, binsha=b"deadbeefdeadbeefdead", message=message)


class FeatureBranchTicketsTest(unittest.TestCase):
    def test_extracts_ticket_from_description(self):
        commit = make_commit(
            "SERVER-1234 Fix a great bug\n\nSome explanation.\nfeature branch ticket: SERVER-1234\n"
        )
        self.assertEqual(feature_branch_tickets(commit), ["SERVER-1234"])

    def test_case_insensitive(self):
        commit = make_commit("SERVER-1234 Fix a great bug\n\nfeature branch ticket: server-1234\n")
        self.assertEqual(feature_branch_tickets(commit), ["SERVER-1234"])

    def test_missing_when_no_description(self):
        commit = make_commit("SERVER-1234 Fix a great bug")
        self.assertEqual(feature_branch_tickets(commit), [])


class CheckFeatureBranchTicketsTest(unittest.TestCase):
    def test_valid(self):
        commits = [
            make_commit("SERVER-1234 Add a feature\n\nfeature branch ticket: SERVER-1234"),
            make_commit("SERVER-1234 Add another feature\n\nfeature branch ticket: SERVER-1234"),
        ]
        self.assertIsNone(check_feature_branch_tickets(commits))

    def test_missing_ticket(self):
        commits = [
            make_commit("SERVER-1234 Add a feature\n\nfeature branch ticket: SERVER-1234"),
            make_commit("SERVER-1234 Add another feature"),
        ]
        self.assertIn("missing it", check_feature_branch_tickets(commits))

    def test_conflicting_tickets(self):
        commits = [
            make_commit("SERVER-1234 Add a feature\n\nfeature branch ticket: SERVER-1234"),
            make_commit("SERVER-5678 Add another feature\n\nfeature branch ticket: SERVER-5678"),
        ]
        self.assertIn("same feature branch ticket", check_feature_branch_tickets(commits))

    def test_no_commits_is_valid(self):
        self.assertIsNone(check_feature_branch_tickets([]))


class CheckBannedStringsTest(unittest.TestCase):
    def test_clean_text(self):
        self.assertIsNone(
            check_banned_strings("SERVER-1234 Fix a great bug\n\nA fine description.")
        )

    def test_banned_string(self):
        self.assertIn(
            "banned string",
            check_banned_strings("SERVER-1234 Fix a great bug\n\nSee https://spruce.mongodb.com"),
        )

    def test_banned_string_ignores_whitespace(self):
        self.assertIn(
            "banned string",
            check_banned_strings("SERVER-1234 Fix\n\nvisit https://\nevergreen.mongodb.com now"),
        )


class FakeResponse:
    def __init__(self, payload):
        self._payload = payload

    def json(self):
        return self._payload


class GetPrCommitsTest(unittest.TestCase):
    @staticmethod
    def gh_page(total_count, nodes, has_next_page, end_cursor=None):
        """Build one GraphQL page of a pull request's commit connection."""
        return {
            "data": {
                "repository": {
                    "pullRequest": {
                        "commits": {
                            "totalCount": total_count,
                            "pageInfo": {
                                "hasNextPage": has_next_page,
                                "endCursor": end_cursor,
                            },
                            "nodes": [
                                {
                                    "commit": {
                                        "oid": oid,
                                        "messageHeadline": headline,
                                        "messageBody": body,
                                    }
                                }
                                for oid, headline, body in nodes
                            ],
                        }
                    }
                }
            }
        }

    @patch("requests.post")
    def test_reassembles_commit_messages(self, mock_request):
        mock_request.return_value = FakeResponse(
            self.gh_page(
                2,
                [
                    ("0" * 40, "SERVER-1234 Add a feature", "feature branch ticket: SERVER-1234"),
                    ("1" * 40, "SERVER-1234 Add another", ""),
                ],
                has_next_page=False,
            )
        )
        commits = get_pr_commits(
            github_org="fun_org", github_repo="fun_repo", pr_number=1234, github_token="fun_token"
        )
        self.assertEqual(len(commits), 2)
        self.assertEqual(
            commits[0].message, "SERVER-1234 Add a feature\nfeature branch ticket: SERVER-1234"
        )
        # A commit with no body keeps a single-line message.
        self.assertEqual(commits[1].message, "SERVER-1234 Add another")
        self.assertEqual(commits[0].binsha, bytes.fromhex("0" * 40))

    @patch("requests.post")
    def test_error_response_exits(self, mock_request):
        """A failed query fails closed: nothing can be validated without the commits."""

        mock_request.return_value = FakeResponse(
            {"data": None, "errors": [{"message": "Bad credentials"}]}
        )
        with self.assertRaises(typer.Exit):
            get_pr_commits(
                github_org="fun_org",
                github_repo="fun_repo",
                pr_number=1234,
                github_token="fun_token",
            )

    @patch("requests.post")
    def test_too_many_commits_exits(self, mock_request):
        mock_request.return_value = FakeResponse(
            self.gh_page(
                1001,
                [("0" * 40, "SERVER-1234", "")],
                has_next_page=False,
            )
        )
        with self.assertRaises(typer.Exit):
            get_pr_commits(
                github_org="fun_org",
                github_repo="fun_repo",
                pr_number=1234,
                github_token="fun_token",
            )

    @patch("requests.post")
    def test_accepts_1000_commits(self, mock_request):
        """The cap is inclusive: a pull request with exactly 1000 commits validates."""
        page_one = self.gh_page(
            1000,
            [(format(oid, "040x"), f"SERVER-1234 Commit {oid}", "") for oid in range(100)],
            has_next_page=True,
            end_cursor="pageOneCursor",
        )
        page_two = self.gh_page(
            1000,
            [(format(oid, "040x"), f"SERVER-1234 Commit {oid}", "") for oid in range(100, 1000)],
            has_next_page=False,
        )
        mock_request.side_effect = [FakeResponse(page_one), FakeResponse(page_two)]
        commits = get_pr_commits(
            github_org="fun_org", github_repo="fun_repo", pr_number=1234, github_token="fun_token"
        )
        self.assertEqual(len(commits), 1000)

    @patch("requests.post")
    def test_null_commit_node_exits(self, mock_request):
        """GitHub returns null nodes for commits it cannot retrieve; fail closed, not crash."""

        class NullNodeResponse:
            def json(self):
                payload = GetPrCommitsTest.gh_page(
                    1,
                    [],
                    has_next_page=False,
                )
                payload["data"]["repository"]["pullRequest"]["commits"]["nodes"] = [None]
                payload["errors"] = [{"message": "Commit could not be retrieved"}]
                return payload

        mock_request.return_value = NullNodeResponse()
        with self.assertRaises(typer.Exit):
            get_pr_commits(
                github_org="fun_org",
                github_repo="fun_repo",
                pr_number=1234,
                github_token="fun_token",
            )

    @patch("requests.post")
    def test_follows_pagination(self, mock_request):
        """Pages beyond the first are fetched with the cursor GitHub returned."""
        mock_request.side_effect = [
            FakeResponse(
                self.gh_page(
                    3,
                    [("0" * 40, "SERVER-1234 Add a feature", "")],
                    has_next_page=True,
                    end_cursor="pageOneCursor",
                )
            ),
            FakeResponse(
                self.gh_page(
                    3,
                    [
                        ("1" * 40, "SERVER-1234 Add another", ""),
                        ("2" * 40, "SERVER-1234 And more", ""),
                    ],
                    has_next_page=False,
                )
            ),
        ]
        commits = get_pr_commits(
            github_org="fun_org", github_repo="fun_repo", pr_number=1234, github_token="fun_token"
        )
        self.assertEqual(len(commits), 3)
        self.assertEqual(len(mock_request.call_args_list), 2)
        second_query = mock_request.call_args_list[1].kwargs["json"]
        self.assertIn("pageOneCursor", second_query["variables"]["cursor"])


if __name__ == "__main__":
    unittest.main()
