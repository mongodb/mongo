"""Unit tests for the buildscripts.ciconfig.tags module."""

import os
import unittest

import buildscripts.resmokelib.testing.tags as _tags

TEST_FILE_PATH = os.path.join(os.path.dirname(__file__), "tags.yml")


class TestTagsConfig(unittest.TestCase):
    """Unit tests for the TagsConfig class."""

    def setUp(self):
        self.conf = _tags.TagsConfig.from_file(TEST_FILE_PATH)

    def test_invalid_path(self):
        invalid_path = "non_existing_file"
        with self.assertRaises(IOError):
            _tags.TagsConfig.from_file(invalid_path)

    def test_list_test_patterns(self):
        patterns = self.conf.get_test_patterns("js_test")
        self.assertEqual(2, len(patterns))
        self.assertIn("jstests/core/example.js", patterns)
        self.assertIn("jstests/core/all*.js", patterns)

    def test_list_test_patterns_unknown_kind(self):
        patterns = self.conf.get_test_patterns("java_test")
        self.assertEqual([], patterns)

    def test_list_tags(self):
        tags = self.conf.get_tags("js_test", "jstests/core/example.js")
        self.assertEqual(3, len(tags))
        self.assertIn("tag1", tags)
        self.assertIn("tag2", tags)
        self.assertIn("tag3", tags)

    def test_list_tags_empty(self):
        tags2 = self.conf.get_tags("cpp_unit_test", "build/*")
        self.assertEqual([], tags2)

    def test_list_tags_unknown_pattern(self):
        tags = self.conf.get_tags("js_test", "jstests/core/unknown.js")
        self.assertEqual([], tags)

    def test_add_tag_to_existing_list(self):
        test_kind = "cpp_unit_test"
        test_pattern = "build/**/auth/*"
        new_tag = "tag100"
        tags = self.conf.get_tags(test_kind, test_pattern)
        self.assertNotIn(new_tag, tags)

        self.conf.add_tag(test_kind, test_pattern, new_tag)

        tags = self.conf.get_tags(test_kind, test_pattern)
        self.assertIn(new_tag, tags)

    def test_add_tag_to_new_list(self):
        test_kind = "js_test"
        test_pattern = "jstests/core/drop.js"
        new_tag = "tag100"
        patterns = self.conf.get_test_patterns(test_kind)
        self.assertNotIn(test_pattern, patterns)

        self.conf.add_tag(test_kind, test_pattern, new_tag)

        tags = self.conf.get_tags(test_kind, test_pattern)
        self.assertIn(new_tag, tags)

    def test_add_tag_to_empty_pattern(self):
        test_kind = "db_test"
        test_pattern = "jstests/disk/quota.js"
        new_tag = "tag2"
        patterns = self.conf.get_test_patterns(test_kind)
        self.assertNotIn(test_pattern, patterns)

        self.conf.add_tag(test_kind, test_pattern, new_tag)

        tags = self.conf.get_tags(test_kind, test_pattern)
        self.assertIn(new_tag, tags)

    def test_tag_order(self):
        test_kind = "js_test"
        test_pattern = "jstests/core/example.js"
        tags = self.conf.get_tags(test_kind, test_pattern)

        self.assertEqual(["tag1", "tag2", "tag3"], tags)

        # Add a tag that should be at the start.
        self.conf.add_tag(test_kind, test_pattern, "tag0")
        tags = self.conf.get_tags(test_kind, test_pattern)
        self.assertEqual(["tag0", "tag1", "tag2", "tag3"], tags)

        # Add a tag that should be in the middle.
        self.conf.add_tag(test_kind, test_pattern, "tag1.5")
        tags = self.conf.get_tags(test_kind, test_pattern)
        self.assertEqual(["tag0", "tag1", "tag1.5", "tag2", "tag3"], tags)

    def test_tag_order_custom_cmp(self):
        test_kind = "js_test"
        test_pattern = "jstests/core/example.js"

        def custom_cmp(tag_a, tag_b):
            a_split = tag_a.split("|")
            b_split = tag_b.split("|")
            return (a_split > b_split) - (a_split < b_split)

        conf = _tags.TagsConfig.from_file(TEST_FILE_PATH, cmp_func=custom_cmp)
        tags = conf.get_tags(test_kind, test_pattern)

        self.assertEqual(["tag1", "tag2", "tag3"], tags)

        # Add a tag that should be at the start.
        conf.add_tag(test_kind, test_pattern, "ta|g2")
        tags = conf.get_tags(test_kind, test_pattern)
        self.assertEqual(["ta|g2", "tag1", "tag2", "tag3"], tags)

        # Add a tag that should be in the middle.
        conf.add_tag(test_kind, test_pattern, "tag1|aaa")
        tags = conf.get_tags(test_kind, test_pattern)
        self.assertEqual(["ta|g2", "tag1", "tag1|aaa", "tag2", "tag3"], tags)
