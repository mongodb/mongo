# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Tests for checkout_idl_files_from_past_releases."""

import unittest
from unittest import mock

import buildscripts.idl.checkout_idl_files_from_past_releases as checkout_idl


class TestGetTags(unittest.TestCase):
    """Test get_tags()."""

    @staticmethod
    def _get_tags(tags: list[str], last_lts: str, last_continuous: str, latest: str) -> list[str]:
        with (
            mock.patch.object(checkout_idl, "check_output", return_value="\n".join(tags).encode()),
            mock.patch.object(checkout_idl, "LAST_LTS_FCV", last_lts),
            mock.patch.object(checkout_idl, "LAST_CONTINUOUS_FCV", last_continuous),
            mock.patch.object(checkout_idl, "LATEST_FCV", latest),
        ):
            return checkout_idl.get_tags()

    def test_prefers_released_tag_over_prerelease(self):
        tags = self._get_tags(
            ["r9.0.0-rc1", "r9.0.0", "r9.1.0-alpha0", "r8.0.0"],
            last_lts="9.0",
            last_continuous="9.0",
            latest="9.1",
        )
        self.assertCountEqual(["r9.0.0", "r9.1.0-alpha0"], tags)

    def test_skips_fcv_with_no_tag(self):
        # A new latest FCV can land before its first release tag is visible to this clone. That FCV
        # must be skipped rather than contributing a None to the tag list.
        tags = self._get_tags(
            ["r9.0.0-rc1", "r8.0.0"], last_lts="9.0", last_continuous="9.0", latest="9.1"
        )
        self.assertEqual(["r9.0.0-rc1"], tags)


if __name__ == "__main__":
    unittest.main()
