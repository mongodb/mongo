"""Unit tests for validate_evergreen_patch_link."""

import unittest
from unittest.mock import patch

from buildscripts.validate_evergreen_patch_link import (
    find_patch_link,
    has_patch_link,
)


class HasPatchLinkTest(unittest.TestCase):
    def test_spruce_version(self):
        self.assertTrue(
            has_patch_link("see https://spruce.mongodb.com/version/abc123_DEF for results")
        )

    def test_spruce_patch(self):
        self.assertTrue(has_patch_link("https://spruce.mongodb.com/patch/abc123"))

    def test_evergreen_version(self):
        self.assertTrue(has_patch_link("https://evergreen.mongodb.com/version/abc123"))

    def test_evergreen_patch(self):
        self.assertTrue(has_patch_link("https://evergreen.mongodb.com/patch/abc123"))

    def test_case_insensitive(self):
        self.assertTrue(has_patch_link("HTTPS://Spruce.MongoDB.com/Version/abc123"))

    def test_empty(self):
        self.assertFalse(has_patch_link(""))
        self.assertFalse(has_patch_link(None))

    def test_unrelated_url(self):
        self.assertFalse(has_patch_link("https://github.com/foo/bar/pull/1"))

    def test_host_only_does_not_match(self):
        self.assertFalse(has_patch_link("see spruce.mongodb.com for details"))


class FindPatchLinkTest(unittest.TestCase):
    def test_link_in_comment(self):
        comments = [{"body": "https://spruce.mongodb.com/version/x"}]
        self.assertTrue(find_patch_link(comments))

    def test_no_link_in_comments(self):
        comments = [{"body": "lgtm"}, {"body": "needs tests"}]
        self.assertFalse(find_patch_link(comments))

    def test_empty_comments(self):
        self.assertFalse(find_patch_link([]))

    def test_link_in_any_comment(self):
        comments = [
            {"body": "lgtm"},
            {"body": "ok: https://spruce.mongodb.com/version/abc"},
        ]
        self.assertTrue(find_patch_link(comments))


class MainFlowTest(unittest.TestCase):
    """Exercise the main() flow end-to-end with the GitHub API stubbed out."""

    @patch("buildscripts.validate_evergreen_patch_link.get_pr_comments")
    def test_passes_when_link_in_comment(self, get_comments):
        from buildscripts.validate_evergreen_patch_link import main

        get_comments.return_value = [{"body": "https://spruce.mongodb.com/version/abc"}]
        # Should not raise.
        main(
            github_org="o",
            github_repo="r",
            pr_number=1,
            github_token="t",
            requester="github_pr",
        )

    @patch("buildscripts.validate_evergreen_patch_link.get_pr_comments")
    def test_fails_when_missing(self, get_comments):
        import typer

        from buildscripts.validate_evergreen_patch_link import main

        get_comments.return_value = [{"body": "looks good"}]

        with self.assertRaises(typer.Exit) as cm:
            main(
                github_org="o",
                github_repo="r",
                pr_number=1,
                github_token="t",
                requester="github_pr",
            )
        self.assertEqual(cm.exception.exit_code, 1)

    @patch("buildscripts.validate_evergreen_patch_link.get_pr_comments")
    def test_skips_when_not_github_pr(self, get_comments):
        from buildscripts.validate_evergreen_patch_link import main

        main(
            github_org="o",
            github_repo="r",
            pr_number=1,
            github_token="t",
            requester="patch",
        )
        get_comments.assert_not_called()


if __name__ == "__main__":
    unittest.main()
