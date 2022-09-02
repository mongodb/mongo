"""Unit tests for the buildscripts.resmokelib.selector module."""

import fnmatch
import os.path
import sys
import unittest
import collections

import buildscripts.resmokelib.config
import buildscripts.resmokelib.parser as parser
import buildscripts.resmokelib.selector as selector
import buildscripts.resmokelib.utils.globstar as globstar

# pylint: disable=protected-access

FIXTURE_PREFIX = "buildscripts/tests/selftest_fixtures"


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
        expression = selector.make_expression({"$allOf": [tag1, tag2]})
        self.assertIsInstance(expression, selector._AllOfExpression)
        self.assertTrue(expression(tags_match))
        self.assertFalse(expression(tags_nomatch))
        self.assertFalse(expression([]))

    def test_anyof_expression(self):
        tag1 = "test_tag"
        tag2 = "other_tag"
        tags_match = [tag1, "third_tag"]
        tags_nomatch = ["third_tag", "some_tag"]
        expression = selector.make_expression({"$anyOf": [tag1, tag2]})
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
        expression = selector.make_expression({"$allOf": [
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
            selector.make_expression({"$anyOf": ["tag1", "tag2"], "invalid": "tag3"})


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

    def test_parse_tag_files_single_file(self):
        tests = (os.path.join(FIXTURE_PREFIX, "one.js"), os.path.join(FIXTURE_PREFIX, "two.js"),
                 os.path.join(FIXTURE_PREFIX, "three.js"))
        expected = collections.defaultdict(list)
        expected[tests[0]] = ["tag1", "tag2", "tag3"]
        expected[tests[1]] = ["tag1", "tag2"]

        tags = self.test_file_explorer.parse_tag_files(
            "js_test", [os.path.join(FIXTURE_PREFIX, "tag_file1.yml")])
        # defaultdict isn't == comparable
        for test in tests:
            self.assertEqual(tags[test], expected[test])

        expected[tests[1]] = ["tag1", "tag2", "tag4"]
        tags = self.test_file_explorer.parse_tag_files(
            "js_test", [os.path.join(FIXTURE_PREFIX, "tag_file2.yml")], tags)
        for test in tests:
            self.assertEqual(tags[test], expected[test])

    def test_parse_tag_files_multiple_file(self):
        tests = (os.path.join(FIXTURE_PREFIX, "one.js"), os.path.join(FIXTURE_PREFIX, "two.js"),
                 os.path.join(FIXTURE_PREFIX, "three.js"))
        expected = collections.defaultdict(list)
        expected[tests[0]] = ["tag1", "tag2", "tag3"]
        expected[tests[1]] = ["tag1", "tag2", "tag4"]

        tags = self.test_file_explorer.parse_tag_files("js_test", [
            os.path.join(FIXTURE_PREFIX, "tag_file1.yml"),
            os.path.join(FIXTURE_PREFIX, "tag_file2.yml")
        ])
        # defaultdict isn't == comparable
        for test in tests:
            self.assertEqual(tags[test], expected[test])


class MockTestFileExplorer(object):
    """Component giving access to mock test files data."""

    NUM_JS_FILES = 4  # Total number of JS files in self.files.

    BINARY = "dbtest"

    def __init__(self):
        self.files = [
            "dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js",
            "dir/subdir3/a/test3a1.js", "build/testA", "build/testB", "build/testC", "dbtest",
            "dbtest.exe"
        ]
        self.tags = {
            "dir/subdir1/test11.js": ["tag1", "tag2"], "dir/subdir1/test12.js": ["tag3"],
            "dir/subdir2/test21.js": ["tag2", "tag4"], "dir/subdir3/a/test3a1.js": ["tag4", "tag5"]
        }
        self.binary = MockTestFileExplorer.BINARY
        self.jstest_tag_file = {"dir/subdir1/test11.js": "tagA", "dir/subdir3/a/test3a1.js": "tagB"}

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

    def read_root_file(self, root_file_path):  # pylint: disable=unused-argument
        return ["build/testA", "build/testB"]

    def fnmatchcase(self, name, pattern):
        return fnmatch.fnmatchcase(name, pattern)

    def isfile(self, path):
        return path in self.files

    def list_dbtests(self, binary):  # pylint: disable=unused-argument
        return ["dbtestA", "dbtestB", "dbtestC"]

    def parse_tag_files(self, test_kind, tag_files=None, tagged_tests=None):  # pylint: disable=unused-argument
        if test_kind == "js_test":
            return self.jstest_tag_file
        return None


class TestTestList(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.test_file_explorer = MockTestFileExplorer()
        cls.tags_from_file = cls.test_file_explorer.jstest_tag_file

    def test_roots(self):
        roots = ["a", "b"]
        test_list = selector._TestList(self.test_file_explorer, roots, tests_are_files=False)
        selected, excluded = test_list.get_tests()
        self.assertEqual(roots, selected)
        self.assertEqual([], excluded)

    def test_roots_normpath(self):
        roots = ["dir/a/abc.js", "dir/b/xyz.js"]
        test_list = selector._TestList(self.test_file_explorer, roots, tests_are_files=False)
        selected, excluded = test_list.get_tests()
        for root_file, selected_file in zip(roots, selected):
            self.assertEqual(os.path.normpath(root_file), selected_file)
        self.assertEqual([], excluded)

    def test_roots_with_glob(self):
        glob_roots = ["dir/subdir1/*.js"]
        expected_roots = ["dir/subdir1/test11.js", "dir/subdir1/test12.js"]
        test_list = selector._TestList(self.test_file_explorer, glob_roots)
        selected, excluded = test_list.get_tests()
        self.assertEqual(expected_roots, selected)
        self.assertEqual([], excluded)

    def test_roots_with_unmatching_glob(self):
        glob_roots = ["unknown/subdir1/*.js"]
        test_list = selector._TestList(self.test_file_explorer, glob_roots)
        selected, excluded = test_list.get_tests()
        self.assertEqual([], selected)
        self.assertEqual([], excluded)

    def test_roots_unknown_file(self):
        roots = ["dir/subdir1/unknown"]
        with self.assertRaisesRegex(ValueError, "Unrecognized test file: dir/subdir1/unknown"):
            selector._TestList(self.test_file_explorer, roots, tests_are_files=True)

    def test_include_files(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_files(["dir/subdir2/test21.js"])
        selected, excluded = test_list.get_tests()
        self.assertEqual(["dir/subdir2/test21.js"], selected)
        self.assertEqual(["dir/subdir1/test11.js", "dir/subdir1/test12.js"], excluded)

    def test_include_files_no_match(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_files(["dir/subdir2/test26.js"])
        selected, excluded = test_list.get_tests()
        self.assertEqual([], selected)
        self.assertEqual(
            ["dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js"], excluded)

    def test_exclude_files(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.exclude_files(["dir/subdir2/test21.js"])
        selected, excluded = test_list.get_tests()
        self.assertEqual(["dir/subdir1/test11.js", "dir/subdir1/test12.js"], selected)
        self.assertEqual(["dir/subdir2/test21.js"], excluded)

    @unittest.skip("Known broken. SERVER-48969 tracks re-enabling.")
    def test_exclude_files_no_match(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        with self.assertRaisesRegex(ValueError, "Unrecognized test file: .*$"):
            test_list.exclude_files(["dir/subdir2/test26.js"])

    def test_exclude_files_glob(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.exclude_files(["dir/subdir2/*.js"])
        selected, excluded = test_list.get_tests()
        self.assertEqual(["dir/subdir1/test11.js", "dir/subdir1/test12.js"], selected)
        self.assertEqual(["dir/subdir2/test21.js"], excluded)

    def test_match_tag_expression(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        expression = selector.make_expression(
            {"$anyOf": [{"$allOf": ["tag1", "tag2"]}, "tag3", {"$allOf": ["tag5", "tag6"]}]})

        def get_tags(test_file):
            return self.test_file_explorer.jstest_tags(test_file)

        test_list.match_tag_expression(expression, get_tags)
        selected, excluded = test_list.get_tests()
        self.assertEqual(["dir/subdir1/test11.js", "dir/subdir1/test12.js"], selected)
        self.assertEqual(["dir/subdir2/test21.js"], excluded)

    def test_include_any_pattern(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*", "dir/subdir3/a/test3a1.js"]
        # 1 pattern and 1 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir/*3/a/*"])
        selected, excluded = test_list.get_tests()
        self.assertEqual(["dir/subdir3/a/test3a1.js"], selected)
        self.assertEqual(
            ["dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js"], excluded)
        # 1 pattern and 0 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir/*4/a/*"])
        selected, excluded = test_list.get_tests()
        self.assertEqual([], selected)
        self.assertEqual([
            "dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js",
            "dir/subdir3/a/test3a1.js"
        ], excluded)
        # 3 patterns and 1 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir/*3/a/*", "notmaching/*", "notmatching2/*.js"])
        selected, excluded = test_list.get_tests()
        self.assertEqual(["dir/subdir3/a/test3a1.js"], selected)
        self.assertEqual(
            ["dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js"], excluded)
        # 3 patterns and 0 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir2/*3/a/*", "notmaching/*", "notmatching2/*.js"])
        selected, excluded = test_list.get_tests()
        self.assertEqual([], selected)
        self.assertEqual([
            "dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js",
            "dir/subdir3/a/test3a1.js"
        ], excluded)
        # 3 patterns and 3 matching
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.include_any_pattern(["dir/*1/*11*", "dir/subdir3/**", "dir/subdir2/*.js"])
        selected, excluded = test_list.get_tests()
        self.assertEqual(
            ["dir/subdir1/test11.js", "dir/subdir2/test21.js", "dir/subdir3/a/test3a1.js"],
            selected)
        self.assertEqual(["dir/subdir1/test12.js"], excluded)

    def test_include_tests_no_force(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.exclude_files(["dir/subdir1/test11.js"])
        test_list.include_files(["dir/subdir1/test11.js"], force=False)
        selected, excluded = test_list.get_tests()
        self.assertEqual([], selected)
        self.assertEqual(
            ["dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js"], excluded)

    def test_include_tests_force(self):
        roots = ["dir/subdir1/*.js", "dir/subdir2/test21.*"]
        test_list = selector._TestList(self.test_file_explorer, roots)
        test_list.exclude_files(["dir/subdir1/test11.js"])
        test_list.include_files(["dir/subdir1/test11.js"], force=True)
        selected, excluded = test_list.get_tests()
        self.assertEqual(["dir/subdir1/test11.js"], selected)
        self.assertEqual(["dir/subdir1/test12.js", "dir/subdir2/test21.js"], excluded)

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

    def test_multi_jstest_selector_config(self):
        sc = selector._MultiJSTestSelectorConfig(roots=["test1", "test2"], group_size=1234,
                                                 group_count_multiplier=5678)
        self.assertEqual(sc.group_size, 1234)
        self.assertEqual(sc.group_count_multiplier, 5678)


class TestSelector(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.selector = selector._Selector(MockTestFileExplorer())

    def test_select_all(self):
        config = selector._SelectorConfig(
            roots=["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"])
        selected, excluded = self.selector.select(config)
        self.assertEqual([
            "dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js",
            "dir/subdir3/a/test3a1.js"
        ], selected)
        self.assertEqual([], excluded)

    def test_select_exclude_files(self):
        config = selector._SelectorConfig(
            roots=["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            exclude_files=["dir/subdir2/test21.js"])
        selected, excluded = self.selector.select(config)
        self.assertEqual(
            ["dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir3/a/test3a1.js"],
            selected)
        self.assertEqual(["dir/subdir2/test21.js"], excluded)

    def test_select_include_files(self):
        config = selector._SelectorConfig(
            roots=["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            include_files=["dir/subdir2/test21.js"])
        selected, excluded = self.selector.select(config)
        self.assertEqual(["dir/subdir2/test21.js"], selected)
        self.assertEqual(
            ["dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir3/a/test3a1.js"],
            excluded)

    def test_select_include_tags(self):
        config = selector._SelectorConfig(
            roots=["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            include_tags="tag1")
        selected, excluded = self.selector.select(config)
        self.assertEqual([], selected)
        self.assertEqual([
            "dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js",
            "dir/subdir3/a/test3a1.js"
        ], excluded)

    def test_select_include_any_tags(self):
        config = selector._SelectorConfig(
            roots=["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            include_with_any_tags=["tag1"])
        selected, excluded = self.selector.select(config)
        self.assertEqual([], selected)
        self.assertEqual([
            "dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js",
            "dir/subdir3/a/test3a1.js"
        ], excluded)


class TestMultiJSSelector(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.selector = selector._MultiJSTestSelector(MockTestFileExplorer())

    @unittest.skip("Known broken. SERVER-48969 tracks re-enabling.")
    def test_multi_js_test_selector_normal(self):
        config = selector._MultiJSTestSelectorConfig(roots=["dir/**/*.js"], group_size=3,
                                                     group_count_multiplier=2)

        selected, _ = self.selector.select(config)
        total = 0

        for group in selected[:-1]:
            self.assertEqual(len(group), 3, "{} did not have 3 unique tests".format(group))
            total += 3

        self.assertLessEqual(
            len(selected[-1]), 3,
            "Last selected group did not have 3 or fewer tests: {}".format(selected[-1]))
        total += len(selected[-1])

        self.assertEqual(total, MockTestFileExplorer.NUM_JS_FILES * config.group_count_multiplier,
                         "The total number of workloads is incorrect")

    @unittest.skip("Known broken. SERVER-48969 tracks re-enabling.")
    def test_multi_js_test_selector_one_group(self):
        """Test we return only one group if the group size equals number of files"""
        num_files = MockTestFileExplorer.NUM_JS_FILES
        config = selector._MultiJSTestSelectorConfig(roots=["dir/**/*.js"], group_size=num_files,
                                                     group_count_multiplier=9999999)
        selected, _ = self.selector.select(config)
        self.assertEqual(len(selected), 1)
        self.assertEqual(len(selected[0]), num_files)


class TestFilterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.test_file_explorer = MockTestFileExplorer()

    def test_unknown_test_kind(self):
        with self.assertRaises(ValueError):
            selector.filter_tests("unknown_test", {})

    def test_cpp_all(self):
        config = {"root": "integrationtest.txt"}
        selected, excluded = selector.filter_tests("cpp_integration_test", config,
                                                   self.test_file_explorer)
        self.assertEqual(["build/testA", "build/testB"], selected)
        self.assertEqual([], excluded)

    def test_cpp_roots_override(self):
        # When roots are specified for cpp tests they override all filtering since
        # 'roots' are populated with the command line arguments.
        config = {"include_files": "unknown_file", "roots": ["build/testC"]}
        selected, excluded = selector.filter_tests("cpp_unit_test", config, self.test_file_explorer)
        self.assertEqual(["build/testC"], selected)
        self.assertEqual([], excluded)
        selected, excluded = selector.filter_tests("cpp_integration_test", config,
                                                   self.test_file_explorer)
        self.assertEqual(["build/testC"], selected)
        self.assertEqual([], excluded)

    def test_cpp_expand_roots(self):
        config = {"root": "integrationtest.txt", "roots": ["build/test*"]}
        selected, excluded = selector.filter_tests("cpp_integration_test", config,
                                                   self.test_file_explorer)
        self.assertEqual(["build/testA", "build/testB", "build/testC"], selected)
        self.assertEqual([], excluded)

        selected, excluded = selector.filter_tests("cpp_unit_test", config, self.test_file_explorer)
        self.assertEqual(["build/testA", "build/testB", "build/testC"], selected)
        self.assertEqual([], excluded)

    def test_cpp_with_any_tags(self):
        buildscripts.resmokelib.config.INCLUDE_WITH_ANY_TAGS = ["tag1"]
        try:
            selector_config = {"root": "unittest.txt"}
            selected, excluded = selector.filter_tests("cpp_unit_test", selector_config,
                                                       test_file_explorer=self.test_file_explorer)
            self.assertEqual([], selected)
            self.assertEqual(["build/testA", "build/testB"], excluded)
        finally:
            buildscripts.resmokelib.config.INCLUDE_WITH_ANY_TAGS = None

    def test_jstest_include_tags(self):
        config = {
            "roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            "include_tags": "tag1"
        }
        selected, excluded = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js"], selected)
        self.assertEqual(
            ["dir/subdir1/test12.js", "dir/subdir2/test21.js", "dir/subdir3/a/test3a1.js"],
            excluded)

    def test_jstest_exclude_tags(self):
        config = {
            "roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            "exclude_tags": "tag1"
        }
        selected, excluded = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(
            ["dir/subdir1/test12.js", "dir/subdir2/test21.js", "dir/subdir3/a/test3a1.js"],
            selected)
        self.assertEqual(["dir/subdir1/test11.js"], excluded)

    def test_jstest_exclude_with_any_tags(self):
        config = {
            "roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            "exclude_with_any_tags": ["tag2"]
        }
        selected, excluded = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js", "dir/subdir2/test21.js"], excluded)
        self.assertEqual(["dir/subdir1/test12.js", "dir/subdir3/a/test3a1.js"], selected)

    @unittest.skip("Known broken. SERVER-48969 tracks re-enabling.")
    def test_filter_temporarily_disabled_tests(self):
        parser.parse_command_line(sys.argv[1:])
        test_file_explorer = MockTestFileExplorer()
        test_file_explorer.tags = {
            "dir/subdir1/test11.js": ["tag1", "tag2", "__TEMPORARILY_DISABLED__"],
            "dir/subdir1/test12.js": ["tag3"], "dir/subdir2/test21.js": ["tag2", "tag4"]
        }
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir2/*.js"]}
        selected, excluded = selector.filter_tests("js_test", config, test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js"], excluded)
        self.assertEqual(["dir/subdir1/test12.js", "dir/subdir2/test21.js"], selected)

    def test_jstest_force_include(self):
        config = {
            "roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            "include_files": ["dir/subdir1/*.js"], "exclude_tags": "tag1"
        }
        selected, excluded = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js", "dir/subdir1/test12.js"], selected)
        self.assertEqual(["dir/subdir2/test21.js", "dir/subdir3/a/test3a1.js"], excluded)

    def test_jstest_all(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"]}
        selected, excluded = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual([
            "dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir2/test21.js",
            "dir/subdir3/a/test3a1.js"
        ], selected)
        self.assertEqual([], excluded)

    def test_jstest_include_with_any_tags(self):
        config = {
            "roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            "include_with_any_tags": ["tag2"]
        }
        selected, excluded = selector.filter_tests("js_test", config, self.test_file_explorer)
        self.assertEqual(["dir/subdir1/test11.js", "dir/subdir2/test21.js"], selected)
        self.assertEqual(["dir/subdir1/test12.js", "dir/subdir3/a/test3a1.js"], excluded)

    def test_jstest_unknown_file(self):
        config = {"roots": ["dir/subdir1/*.js", "dir/subdir1/unknown"]}
        with self.assertRaisesRegex(ValueError, "Unrecognized test file: dir/subdir1/unknown"):
            selector.filter_tests("js_test", config, self.test_file_explorer)

    def test_json_schema_exclude_files(self):
        config = {
            "roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            "exclude_files": ["dir/subdir2/test21.js"]
        }
        selected, excluded = selector.filter_tests("json_schema_test", config,
                                                   self.test_file_explorer)
        self.assertEqual(
            ["dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir3/a/test3a1.js"],
            selected)
        self.assertEqual(["dir/subdir2/test21.js"], excluded)

    def test_json_schema_include_files(self):
        config = {
            "roots": ["dir/subdir1/*.js", "dir/subdir2/*.js", "dir/subdir3/a/*.js"],
            "include_files": ["dir/subdir2/test21.js"]
        }
        selected, excluded = selector.filter_tests("json_schema_test", config,
                                                   self.test_file_explorer)
        self.assertEqual(["dir/subdir2/test21.js"], selected)
        self.assertEqual(
            ["dir/subdir1/test11.js", "dir/subdir1/test12.js", "dir/subdir3/a/test3a1.js"],
            excluded)

    @unittest.skipUnless(
        os.path.exists(MockTestFileExplorer.BINARY),
        "{} not built".format(MockTestFileExplorer.BINARY))
    def test_db_tests_all(self):
        config = {"binary": self.test_file_explorer.binary}
        selected, excluded = selector.filter_tests("db_test", config, self.test_file_explorer)
        self.assertEqual(["dbtestA", "dbtestB", "dbtestC"], selected)
        self.assertEqual([], excluded)

    def test_db_tests_roots_override(self):
        # When roots are specified for db_tests they override all filtering since
        # 'roots' are populated with the command line arguments.
        config = {
            "binary": self.test_file_explorer.binary, "include_suites": ["dbtestB"],
            "roots": ["dbtestOverride"]
        }
        selected, excluded = selector.filter_tests("db_test", config, self.test_file_explorer)
        self.assertEqual(["dbtestOverride"], selected)
        self.assertEqual([], excluded)

    @unittest.skipUnless(
        os.path.exists(MockTestFileExplorer.BINARY),
        "{} not built".format(MockTestFileExplorer.BINARY))
    def test_db_tests_include_suites(self):
        config = {"binary": self.test_file_explorer.binary, "include_suites": ["dbtestB"]}
        selected, excluded = selector.filter_tests("db_test", config, self.test_file_explorer)
        self.assertEqual(["dbtestB"], selected)
        self.assertEqual(["dbtestA", "dbtestC"], excluded)
