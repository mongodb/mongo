"""Verify the Mothra S3 team export produces the same assignment tags as the Mothra clone.

TODO(DEVPROD-41449): Remove this test once the S3 export is stable, prior to YAML deletion.

`generate_result_tasks.resolve_assignment_tags()` reads the mapping from GitHub team name
to `assigned_to_jira_team_*` tag out of the Mothra S3 export, falling back to the Mothra
repo cloned into the workspace (exposed to Bazel as `@mothra//:teams`) when S3 is
unavailable. This test asserts the two sources agree, so that we learn about drift instead
of silently generating different tags depending on which source was used.

This test needs both the Mothra clone in the runfiles and AWS credentials for the Mothra
export reader role. It skips when either is unavailable so that it stays a no-op for local
runs.
"""

import os
import unittest

from buildscripts.generate_result_tasks import (
    resolve_assignment_tags_from_clone,
    resolve_assignment_tags_from_s3,
)


def have_aws_credentials() -> bool:
    return bool(os.environ.get("AWS_ACCESS_KEY_ID")) or bool(
        os.environ.get("AWS_WEB_IDENTITY_TOKEN_FILE")
    )


class TestMothraAssignmentTags(unittest.TestCase):
    def test_s3_export_matches_mothra_clone(self):
        if not have_aws_credentials():
            self.skipTest("No AWS credentials for the Mothra export reader role.")

        clone_tags = resolve_assignment_tags_from_clone()
        if not clone_tags:
            # @mothra//:teams falls back to an empty stub when 10gen/mothra has not been
            # cloned into the workspace. There is nothing to compare against.
            self.skipTest("The Mothra repo is not cloned into the workspace.")

        s3_tags = resolve_assignment_tags_from_s3()

        self.assertEqual(
            s3_tags,
            clone_tags,
            "Mothra S3 export and Mothra clone disagree on assignment tags. "
            f"Only in clone: {sorted(set(clone_tags) - set(s3_tags))}. "
            f"Only in S3: {sorted(set(s3_tags) - set(clone_tags))}. "
            "Changed: "
            + str(
                {
                    name: {"clone": clone_tags[name], "s3": s3_tags[name]}
                    for name in sorted(set(clone_tags) & set(s3_tags))
                    if clone_tags[name] != s3_tags[name]
                }
            ),
        )


if __name__ == "__main__":
    unittest.main()
