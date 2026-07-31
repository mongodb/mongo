"""Unit tests for feature_flag_tags_check.py."""

import unittest
from unittest.mock import patch

from buildscripts import feature_flag_tags_check


class TestFindTestsInGitDiff(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.requires_fcv_tag = "requires_fcv_51"
        cls.original_requires_fcv_tag = feature_flag_tags_check.REQUIRES_FCV_TAG_LATEST
        feature_flag_tags_check.REQUIRES_FCV_TAG_LATEST = cls.requires_fcv_tag

    @classmethod
    def tearDownClass(cls):
        feature_flag_tags_check.REQUIRES_FCV_TAG_LATEST = cls.original_requires_fcv_tag

    def test_get_tests_missing_fcv_tag_no_tag(self):
        tests = ["dummy_jstest_file.js"]
        with patch.object(feature_flag_tags_check.jscomment, "get_tags", return_value=[]):
            result = feature_flag_tags_check.get_tests_missing_fcv_tag(tests)
        self.assertCountEqual(tests, result)

    def test_get_tests_missing_fcv_tag_have_tag(self):
        tests = ["dummy_jstest_file.js"]
        with patch.object(
            feature_flag_tags_check.jscomment, "get_tags", return_value=[self.requires_fcv_tag]
        ):
            result = feature_flag_tags_check.get_tests_missing_fcv_tag(tests)
        self.assertCountEqual([], result)

    def test_get_tests_missing_fcv_tag_test_file_deleted(self):
        tests = ["some/non/existent/jstest_file.js"]
        result = feature_flag_tags_check.get_tests_missing_fcv_tag(tests)
        self.assertCountEqual([], result)


class TestGetTestsWithStaleFeatureFlagTag(unittest.TestCase):
    def test_stale_tag_reported(self):
        tests = ["dummy_jstest_file.js"]
        with patch.object(
            feature_flag_tags_check.jscomment, "get_tags", return_value=["featureFlagToaster"]
        ):
            result = feature_flag_tags_check.get_tests_with_stale_feature_flag_tag(
                tests, ["featureFlagToaster", "featureFlagSpoon"]
            )
        self.assertCountEqual(["dummy_jstest_file.js: featureFlagToaster"], result)

    def test_tag_removed(self):
        tests = ["dummy_jstest_file.js"]
        with patch.object(
            feature_flag_tags_check.jscomment, "get_tags", return_value=["requires_fcv_51"]
        ):
            result = feature_flag_tags_check.get_tests_with_stale_feature_flag_tag(
                tests, ["featureFlagToaster"]
            )
        self.assertCountEqual([], result)

    def test_test_file_deleted(self):
        tests = ["some/non/existent/jstest_file.js"]
        result = feature_flag_tags_check.get_tests_with_stale_feature_flag_tag(
            tests, ["featureFlagToaster"]
        )
        self.assertCountEqual([], result)


if __name__ == "__main__":
    unittest.main()
