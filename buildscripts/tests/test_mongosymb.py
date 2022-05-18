"""Unit tests for buildscripts/mongosymb.py."""
# pylint: disable=missing-docstring
import unittest

from buildscripts import mongosymb as under_test


class TestParseVersion(unittest.TestCase):
    def test_parse_version_with_patch(self):
        trace_doc = {
            "processInfo": {
                "mongodbVersion": "6.0.0-alpha0-37-ge1d28c1-patch-6257e60a32f417196bc25169"
            }
        }
        version = under_test.parse_version(trace_doc)
        self.assertEqual(version, "6257e60a32f417196bc25169")

    def test_parse_version_without_patch(self):
        trace_doc = {"processInfo": {"mongodbVersion": "6.1.0-alpha-504-g0c8a142"}}
        version = under_test.parse_version(trace_doc)
        self.assertEqual(version, None)

    def test_parse_version_no_mongodb_version(self):
        trace_doc = {"processInfo": {}}
        version = under_test.parse_version(trace_doc)
        self.assertEqual(version, None)

    def test_parse_version_no_process_info(self):
        trace_doc = {}
        version = under_test.parse_version(trace_doc)
        self.assertEqual(version, None)


class TestHasHighNotFoundPathsRatio(unittest.TestCase):
    def test_not_found_paths_ratio_is_more_than_0_5(self):
        frames = [
            {"path": "some/path"},
            {"path": "some/path"},
            {"path": "some/path"},
            {"path": None},
        ]
        ret = under_test.has_high_not_found_paths_ratio(frames)
        self.assertEqual(ret, False)

    def test_not_found_paths_ratio_is_equal_to_0_5(self):
        frames = [
            {"path": "some/path"},
            {"path": "some/path"},
            {"path": None},
            {"path": None},
        ]
        ret = under_test.has_high_not_found_paths_ratio(frames)
        self.assertEqual(ret, True)

    def test_not_found_paths_ratio_is_less_than_0_5(self):
        frames = [
            {"path": "some/path"},
            {"path": None},
            {"path": None},
            {"path": None},
        ]
        ret = under_test.has_high_not_found_paths_ratio(frames)
        self.assertEqual(ret, True)

    def test_no_frames(self):
        frames = []
        ret = under_test.has_high_not_found_paths_ratio(frames)
        self.assertEqual(ret, False)
