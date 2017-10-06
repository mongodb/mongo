
"""Unit tests for the buildscripts.resmokelib.selector module."""

from __future__ import absolute_import

import fnmatch
import unittest

import buildscripts.resmokelib.selector as selector
import buildscripts.resmokelib.utils.globstar as globstar
import buildscripts.resmokelib.config
import buildscripts.resmokeconfig


class TestExpressions(unittest.TestCase):
    """Unit tests for the tag matching expressions."""

    def test_match_expression(self):
        tag = "test_tag"
        tags_with = ["other_tag", tag]
        tags_without = ["other_tag", "some_tag"]
        expression = selector.make_expression(tag)
        self.assertIsInstance(expression, selector._MatchExpression)
        self.assertTrue(expression(tags_with))
        self.assertFalse(expression(tags_without))
        self.assertFalse(expression([]))

    def test_allof_expression(self):
        tag1 = "test_tag"
        tag2 = "other_tag"
        tags_match = [tag2, tag1, "third_tag"]
        tags_nomatch = [tag2, "some_tag"]
        expression = selector.make_expression({
            "$allOf": [tag1, tag2]})
        self.assertIsInstance(expression, selector._AllOfExpression)
        self.assertTrue(expression(tags_match))
        self.assertFalse(expression(tags_nomatch))
        self.assertFalse(expression([]))

    def test_anyof_expression(self):
        tag1 = "test_tag"
        tag2 = "other_tag"
        tags_match = [tag1, "third_tag"]
        tags_nomatch = ["third_tag", "some_tag"]
        expression = selector.make_expression({
            "$anyOf": [tag1, tag2]})
        self.assertIsInstance(expression, selector._AnyOfExpression)
        self.assertTrue(expression(tags_match))
        self.assertFalse(expression(tags_nomatch))
        self.assertFalse(expression([]))

    def test_not_expression(self):
        tag = "test_tag"
        tags_match = ["other_tag_1"]
        tags_nomatch = ["other_tag_1", tag]
        expression = selector.make_expression({"$not": tag})
        self.assertIsInstance(expression, selector._NotExpression)
        self.assertTrue(expression(tags_match))
        self.assertTrue(expression([]))
        self.assertFalse(expression(tags_nomatch))

    def test_allof_anyof_expression(self):
        tag1 = "test_tag_1"
        tag2 = "test_tag_2"
        tag3 = "test_tag_3"
        tags_match_1 = [tag1, tag3]
        tags_match_2 = [tag2, tag3]
        tags_nomatch_1 = ["other_tag_1", tag3]
        tags_nomatch_2 = [tag1, "other_tag_2"]
        tags_nomatch_3 = [tag2, "other_tag_2"]
        tags_nomatch_4 = [tag2]
        tags_nomatch_5 = ["other_tag_2"]
        expression = selector.make_expression({
            "$allOf": [
                {"$anyOf": [tag1, tag2]},
                tag3,
            ]})
        self.assertIsInstance(expression, selector._AllOfExpression)
        self.assertTrue(expression(tags_match_1))
        self.assertTrue(expression(tags_match_2))
        self.assertFalse(expression(tags_nomatch_1))
        self.assertFalse(expression(tags_nomatch_2))
        self.assertFalse(expression(tags_nomatch_3))
        self.assertFalse(expression(tags_nomatch_4))
        self.assertFalse(expression(tags_nomatch_5))
        self.assertFalse(expression([]))

    def test_invalid_expression(self):
        with self.assertRaises(ValueError):
            selector.make_expression({"invalid": ["tag1", "tag2"]})
        with self.assertRaises(ValueError):
            selector.make_expression({"$anyOf": ["tag1", "tag2"],
                                      "invalid": "tag3"})


class TestTestFileExplorer(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.test_file_explorer = selector.TestFileExplorer()

    def test_is_glob_pattern(self):
        self.assertTrue(self.test_file_explorer.is_glob_pattern("directory/*file.js"))
        self.assertFalse(self.test_file_explorer.is_glob_pattern("directory/file.js"))

    def test_fnmatchcase(self):
        pattern = "dir*/file.js"
        self.assertTrue(self.test_file_explorer.fnmatchcase("directory/file.js", pattern))
        self.assertFalse(self.test_file_explorer.fnmatchcase("other/file.js", pattern))


class MockTestFileExplorer(object):
    """Component giving access to mock test files data."""
    def __init__(self):
        self.files = ["dir/subdir1/test11.js",
                      "dir/subdir1/test12.js",
                      "dir/subdir2/test21.js",
                      "dir/subdir3/a/test3a1.js",
                      "build/testA",
                      "build/testB",
                      "build/testC",
                      "dbtest"]
        self.tags = {"dir/subdir1/test11.js": ["tag1", "tag2"],
                     "dir/subdir1/test12.js": ["tag3"],
                     "dir/subdir2/test21.js": ["tag2", "tag4"],
                     "dir/subdir3/a/test3a1.js": ["tag4", "tag5"]}
        self.binary = "dbtest"
        self.jstest_tag_file = {"dir/subdir1/test11.js": "tagA",
                                "dir/subdir3/a/test3a1.js": "tagB"}

    def is_glob_pattern(self, pattern):
        return globstar.is_glob_pattern(pattern)

    def iglob(self, pattern):
        globbed = []
        for test_file in self.files:
            if fnmatch.fnmatchcase(test_file, pattern):
                globbed.append(test_file)
        return globbed

    def jstest_tags(self, file_path):
        return self.tags.get(file_path, [])

    def read_root_file(self, root_file_path):
        return ["build/testA", "build/testB"]

    def fnmatchcase(self, name, pattern):
        return fnmatch.fnmatchcase(name, pattern)

    def isfile(self, path):
        return path in self.files

    def list_dbtests(self, binary):
        return ["dbtestA", "dbtestB", "dbtestC"]

    def parse_tag_file(self, test_kind):
        if test_kind == "js_test":
            return self.jstest_tag_file


class TestTestList(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.test_file_explorer = MockTestFileExplorer()
        cls.tags_from_file = cls.test_file_explorer.jstest_tag_file

    def test_roots(self):
        roots = ["a", "b"]
        test_list = selector._TestList(self.test_file_explorer, roots, tests_are_files=False)
        self.assertEqual(roots, test_list.get_tests())

    def test_roots_with_glob(self):
        glob_roots = ["dir/subdir1/*.js"]
        expected_roots = ["dir/subdir1/test11.js", "dir/subdir1/test12.js"]
        test_list = selector._TestList(self.test_file_explorer, glob_roots)
        self.assertEqual(expected_roots, test_list.get_tests())

    def test_roots_with_unmatching_glob(self):
        glob_roots = ["unknown/subdir1/*.js"]
        with self.assertRaisesRegexp(ValueError, "Pattern does not match any files: unknown/subdir1/\*.js"):
            selector._TestList(self.test_file_explorer, glob_roots)

    def test_roots_unknown_file(self):
        roots = ["dir/subdir1/unknown"]
        with self.assertRaisesRegexp(ValueError, "Unrecognized test file: dir/subdir1/unknown"):
            selector._TestList(self.test_file_explorer, roots, tests_are_files=True)

    def test_include_files(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_files(["dir/subdir2/test21.js"])
        self.assertEqual(["dir/subdir2/test21.js"], test_list.get_tests())

    def test_include_files_no_match(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_files(["dir/subdir2/test26.js"])
        self.assertEqual([], test_list.get_tests())

    def test_exclude_files(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.exclude_files(["dir/subdir2/test21.js"])
        self.assertEqual(["dir/subdir1/test11.js",
                          "dir/subdir1/test12.js"], test_list.get_tests())

    def test_exclude_files_no_match(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        with self.assertRaisesRegexp(ValueError, "Unrecognized test file: .*$"):
            test_list.exclude_files(["dir/subdir2/test26.js"])

    def test_exclude_files_glob(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.exclude_files(["dir/subdir2/*.js"])
        self.assertEqual(["dir/subdir1/test11.js",
                          "dir/subdir1/test12.js"], test_list.get_tests())

    def test_match_tag_expression(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        expression = selector.make_expression({"$anyOf": [
            {"$allOf": ["tag1", "tag2"]},
            "tag3",
            {"$allOf": ["tag5", "tag6"]}]})

        def get_tags(test_file):
            return self.test_file_explorer.jstest_tags(test_file)

        test_list.match_tag_expression(expression, get_tags)
        self.assertEqual(["dir/subdir1/test11.js",
                          "dir/subdir1/test12.js"], test_list.get_tests())

    def test_include_any_pattern(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*", "dir/subdir3/a/test3a1.js"]
        # 1 pattern and 1 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir/*3/a/*"])
        self.assertEqual(["dir/subdir3/a/test3a1.js"], test_list.get_tests())
        # 1 pattern and 0 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir/*4/a/*"])
        self.assertEqual([], test_list.get_tests())
        # 3 patterns and 1 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir/*3/a/*", "notmaching/*", "notmatching2/*.js"])
        self.assertEqual(["dir/subdir3/a/test3a1.js"], test_list.get_tests())
        # 3 patterns and 0 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir2/*3/a/*", "notmaching/*", "notmatching2/*.js"])
        self.assertEqual([], test_list.get_tests())
        # 3 patterns and 3 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir/*1/*11*", "dir/subdir3/**", "dir/subdir2/*.js"])
        self.assertEqual(["dir/subdir1/test11.js", "dir/subdir2/test21.js",
                          "dir/subdir3/a/test3a1.js"],
                         test_list.get_tests())

    def test_include_tests_no_force(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.exclude_files(["dir/subdir1/test11.js"])
        test_list.include_files(["dir/subdir1/test11.js"], force=False)
        self.assertEqual([], test_list.get_tests())

    def test_include_tests_force(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.exclude_files(["dir/subdir1/test11.js"])
        test_list.include_files(["dir/subdir1/test11.js"], force=True)
        self.assertEqual(["dir/subdir1/test11.js"], test_list.get_tests())

    def test_tests_are_not_files(self):
        roots = ["a", "b"]
        test_list = selector._TestList(self.test_file_explorer, roots, tests_are_files=False)
        with self.assertRaises(TypeError):
            test_list.include_files([])
        with self.assertRaises(TypeError):
            test_list.exclude_files([])


class TestSelectorConfig(unittest.TestCase):
    def test_root_roots(self):
        with self.assertRaises(ValueError):
            selector._SelectorConfig(root="path_to_root", roots=["test1", "test2"])

    def test_include_exclude_tags(self):
        with self.assertRaises(ValueError):
            selector._SelectorConfig(include_tags="tag1", exclude_tags="tag2")


class TestSelector(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.selector = selector._Selector(MockTestFileExplorer())

    def test_select_all(self):
        config = selector._SelectorConfig(roots=["dir/subdir1/*.js", "dir/subdir2/*.js",
                                                 "dir/subdir3/a/*.js"])
        selected = self.selector.select(config)
        self.assertEqual(["dir/subdir1/test11.js",
                          "dir/subdir1/test12.js",
                          "dir/subdir2/test21.js",
                          "dir/subdir3/a/test3a1.js"], selected)

    def test_select_exclude_files(self):
        config = selector._SelectorConfig(roots=["dir/subdir1/*.js", "dir/subdir2/*.js",
                                                 "dir/subdir3/a/*.js"],
                                          exclude_files=["dir/subdir2/test21.js"])
        selected = self.selector.select(config)
        self.assertEqual(["dir/subdir1/test11.js",
                          "dir/subdir1/test12.js",
                          "dir/subdir3/a/test3a1.js"], selected)

    def test_select_include_files(self):
        config = selector._SelectorConfig(roots=["dir/subdir1/*.js", "dir/subdir2/*.js",
                                                 "dir/subdir3/a/*.js"],
                                          include_files=["dir/subdir2/test21.js"])
        selected = self.selector.select(config)
        self.assertEqual(["dir/subdir2/test21.js"], selected)

    def test_select_include_tags(self):
        config = selector._SelectorConfig(roots=["dir/subdir1/*.js", "dir/subdir2/*.js",
                                                 "dir/subdir3/a/*.js"],
                                          include_tags="tag1")
        selected = self.selector.select(config)
        self.assertEqual([], selected)

    def test_select_include_any_tags(self):
        config = selector._SelectorConfig(roots=["dir/subdir1/*.js", "dir/subdir2/*.js",
                                                 "dir/subdir3/a/*.js"],
                                          include_with_any_tags=["tag1"])
        selected = self.selector.select(config)
        self.assertEqual([], selected)


class TestFilterTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.test_file_explorer = MockTestFileExplorer()

    def test_unknown_test_kind(self):
        with self.assertRaises(ValueError):
            selector.filter_tests("unknown_test", {})

    def test_cpp_all(self):
        config = {"root": "integrationtest.txt"}
        selected = selector.filter_tests("cpp_integration_test", config, self.test_file_explorer)
        self.assertEqual(["build/testA", "build/testB"], selected)

    def test_cpp_roots_override(self):
        # When roots are specified for cpp tests they override all filtering since
        # 'roots' are populated with the command line arguments.
        config = {"include_files": "unknown_file",
                  "roots": ["build/testC"]}
        selected = selector.filter_tests("cpp_unit_test", config, self.test_file_explorer)
        self.assertEqual(["build/testC"], selected)
        selected = selector.filter_tests("cpp_integration_test", config, self.test_file_explorer)
        self.assertEqual(["build/testC"], selected)

    def test_cpp_expand_roots(self):
        config = {"root": "integrationtest.txt", "roots": ["build/test*"]}
        selected = selector.filter_tests("cpp_integration_test", config, self.test_file_explorer)
        self.assertEqual(["build/testA", "build/testB", "build/testC"], selected)

        selected = selector.filter_tests("cpp_unit_test", config, self.test_file_explorer)
        self.assertEqual(["build/testA", "build/testB", "build/testC"], selected)

    def test_cpp_with_any_tags(self):
        buildscripts.resmokelib.config.INCLUDE_WITH_ANY_TAGS = ["tag1"]
        try:
            selector_config = {"root": "unittest.txt"}
            selected = selector.filter_tests(
                "cpp_unit_test",
                selector_config,
                test_file_explorer=self.test_file_explorer)
            self.assertEqual([], selected)
        finally:
            buildscripts.resmokelib.config.INCLUDE_WITH_ANY_TAGS = None

    def test_jstest_include_tags(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
                  "include_tags": "tag1"}
        selected = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js"], selected)

    def test_jstest_exclude_tags(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
                  "exclude_tags": "tag1"}
        selected = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test12.js",
                          "dir/subdir2/test21.js",
                          "dir/subdir3/a/test3a1.js"], selected)

    def test_jstest_force_include(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
                  "include_files": ["dir/subdir1/*.js"],
                  "exclude_tags": "tag1"}
        selected = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js",
                          "dir/subdir1/test12.js"], selected)

    def test_jstest_all(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"]}
        selected = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js",
                          "dir/subdir1/test12.js",
                          "dir/subdir2/test21.js",
                          "dir/subdir3/a/test3a1.js"], selected)

    def test_jstest_include_with_any_tags(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
                  "include_with_any_tags": ["tag2"]}
        selected = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js",
                          "dir/subdir2/test21.js"], selected)

    def test_jstest_unknown_file(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir1/unknown"]}
        with self.assertRaisesRegexp(ValueError, "Unrecognized test file: dir/subdir1/unknown"):
            selector.filter_tests("js_test", config, self.test_file_explorer)

    def test_json_schema_exclude_files(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
                  "exclude_files": ["dir/subdir2/test21.js"]}
        selected = selector.filter_tests("json_schema_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js",
                          "dir/subdir1/test12.js",
                          "dir/subdir3/a/test3a1.js"], selected)

    def test_json_shcema_include_files(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
                  "include_files": ["dir/subdir2/test21.js"]}
        selected = selector.filter_tests("json_schema_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir2/test21.js"], selected)

    def test_db_tests_all(self):
        config = {"binary": self.test_file_explorer.binary}
        selected = selector.filter_tests("db_test", config, self.test_file_explorer)
        self.assertEqual(["dbtestA", "dbtestB", "dbtestC"], selected)

    def test_db_tests_roots_override(self):
        # When roots are specified for db_tests they override all filtering since
        # 'roots' are populated with the command line arguments.
        config = {"binary": self.test_file_explorer.binary,
                  "include_suites": ["dbtestB"],
                  "roots": ["dbtestOverride"]}
        selected = selector.filter_tests("db_test", config, self.test_file_explorer)
        self.assertEqual(["dbtestOverride"], selected)

    def test_db_tests_include_suites(self):
        config = {"binary": self.test_file_explorer.binary,
                  "include_suites": ["dbtestB"]}
        selected = selector.filter_tests("db_test", config, self.test_file_explorer)
        self.assertEqual(["dbtestB"], selected)


if __name__ == "__main__":
    unittest.main()
