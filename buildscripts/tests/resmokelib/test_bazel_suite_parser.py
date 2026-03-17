"""Unit tests for buildscripts/resmokelib/bazel_suite_parser.py."""

import os
import tempfile
import unittest

from buildscripts.resmokelib.bazel_suite_parser import (
    _extract_attribute,
    _parse_load_statements,
    _resolve_identifier_to_labels,
)


class TestExtractAttribute(unittest.TestCase):
    """Unit tests for the _extract_attribute() function."""

    def test_simple_multiline_list(self):
        """Test extraction of a simple multi-line list."""
        block = """resmoke_suite_test(
    name = "test_suite",
    srcs = [
        "//jstests/core:all_subpackage_javascript_files",
        "//jstests/fle2:all_subpackage_javascript_files",
        "//src/mongo/db/modules/enterprise/jstests/fle2:all_subpackage_javascript_files",
    ],
    shard_count = 2,
)"""
        result = _extract_attribute(block, "srcs")
        self.assertEqual(
            result,
            [
                "//jstests/core:all_subpackage_javascript_files",
                "//jstests/fle2:all_subpackage_javascript_files",
                "//src/mongo/db/modules/enterprise/jstests/fle2:all_subpackage_javascript_files",
            ],
        )

    def test_multiline_list_with_comments(self):
        """Test extraction of multi-line list with inline comments."""
        block = """resmoke_suite_test(
    name = "test_suite",
    srcs = [
        "//jstests/sharding/libs:all_subpackage_javascript_files",

        # comment
        "//jstests/sharding/analyze_shard_key/libs:all_subpackage_javascript_files",
        "//jstests/sharding/internal_txns/libs:all_subpackage_javascript_files",
        "//jstests/sharding/updateOne_without_shard_key/libs:all_subpackage_javascript_files",
    ],
    shard_count = 2,
)"""
        result = _extract_attribute(block, "srcs")
        self.assertEqual(
            result,
            [
                "//jstests/sharding/libs:all_subpackage_javascript_files",
                "//jstests/sharding/analyze_shard_key/libs:all_subpackage_javascript_files",
                "//jstests/sharding/internal_txns/libs:all_subpackage_javascript_files",
                "//jstests/sharding/updateOne_without_shard_key/libs:all_subpackage_javascript_files",
            ],
        )

    def test_single_line_list(self):
        """Test extraction of a single-line list."""
        block = """resmoke_suite_test(
    name = "test_suite",
    srcs = ["//jstests/core:test.js", "//jstests/core:test2.js"],
    shard_count = 2,
)"""
        result = _extract_attribute(block, "srcs")
        self.assertEqual(result, ["//jstests/core:test.js", "//jstests/core:test2.js"])

    def test_attribute_not_found(self):
        """Test behavior when attribute is not present."""
        block = """resmoke_suite_test(
    name = "test_suite",
    shard_count = 2,
)"""
        result = _extract_attribute(block, "srcs")
        self.assertEqual(result, [])

    def test_mixed_quotes(self):
        """Test extraction with mixed single and double quotes."""
        block = """resmoke_suite_test(
    name = "test_suite",
    srcs = [
        "//jstests/core:test.js",
        '//jstests/core:test2.js',
    ],
    shard_count = 2,
)"""
        result = _extract_attribute(block, "srcs")
        self.assertEqual(result, ["//jstests/core:test.js", "//jstests/core:test2.js"])

    def test_empty_list(self):
        """Test extraction of an empty list."""
        block = """resmoke_suite_test(
    name = "test_suite",
    srcs = [],
    shard_count = 2,
)"""
        result = _extract_attribute(block, "srcs")
        self.assertEqual(result, [])

    def test_multiple_attributes(self):
        """Test extraction when multiple attributes are present."""
        block = """resmoke_suite_test(
    name = "test_suite",
    srcs = [
        "//jstests/core:test.js",
    ],
    deps = [
        "//some:dependency",
    ],
    shard_count = 2,
)"""
        srcs_result = _extract_attribute(block, "srcs")
        deps_result = _extract_attribute(block, "deps")
        self.assertEqual(srcs_result, ["//jstests/core:test.js"])
        self.assertEqual(deps_result, ["//some:dependency"])


class TestIdentifierResolution(unittest.TestCase):
    """Unit tests for identifier resolution functionality."""

    def setUp(self):
        """Create temporary directory for test .bzl files."""
        self.test_dir = tempfile.mkdtemp()

    def tearDown(self):
        """Clean up temporary directory."""
        import shutil

        shutil.rmtree(self.test_dir)

    def test_parse_load_statements_single_line(self):
        """Test parsing a single-line load statement."""
        content = 'load("//jstests/suites:selectors.bzl", "sharding_srcs", "core_srcs")'
        result = _parse_load_statements(content, "buildscripts/resmokeconfig")

        expected_path = os.path.join(os.getcwd(), "jstests", "suites", "selectors.bzl")
        self.assertEqual(result["sharding_srcs"], expected_path)
        self.assertEqual(result["core_srcs"], expected_path)

    def test_parse_load_statements_multiple_loads(self):
        """Test parsing multiple load statements."""
        content = """
load("//jstests/suites:selectors.bzl", "sharding_srcs")
load("//jstests/core:tests.bzl", "core_tests")
"""
        result = _parse_load_statements(content, "buildscripts/resmokeconfig")

        self.assertEqual(
            result["sharding_srcs"], os.path.join(os.getcwd(), "jstests", "suites", "selectors.bzl")
        )
        self.assertEqual(
            result["core_tests"], os.path.join(os.getcwd(), "jstests", "core", "tests.bzl")
        )

    def test_parse_load_statements_relative_path(self):
        """Test parsing load statement with relative path."""
        content = 'load(":local_defs.bzl", "local_srcs")'
        result = _parse_load_statements(content, "buildscripts/resmokeconfig")

        # Package-relative path: :local_defs.bzl resolves relative to package directory
        expected_path = os.path.join("buildscripts", "resmokeconfig", "local_defs.bzl")
        self.assertEqual(result["local_srcs"], expected_path)

    def test_resolve_identifier_simple(self):
        """Test resolving a simple identifier to list of labels."""
        # Create a .bzl file with an identifier definition
        bzl_file = os.path.join(self.test_dir, "test_srcs.bzl")
        with open(bzl_file, "w") as f:
            f.write(
                """
test_files = [
    "//jstests/core:test1.js",
    "//jstests/core:test2.js",
    "//jstests/core:test3.js",
]
"""
            )

        identifier_to_bzl_file = {"test_files": bzl_file}
        result = _resolve_identifier_to_labels("test_files", identifier_to_bzl_file)

        self.assertEqual(
            result,
            [
                "//jstests/core:test1.js",
                "//jstests/core:test2.js",
                "//jstests/core:test3.js",
            ],
        )

    def test_resolve_identifier_with_comments(self):
        """Test resolving identifier with comments in the list."""
        bzl_file = os.path.join(self.test_dir, "test_srcs.bzl")
        with open(bzl_file, "w") as f:
            f.write(
                """
test_files = [
    "//jstests/core:test1.js",
    # This is a comment
    "//jstests/core:test2.js",
    "//jstests/core:test3.js",  # inline comment
]
"""
            )

        identifier_to_bzl_file = {"test_files": bzl_file}
        result = _resolve_identifier_to_labels("test_files", identifier_to_bzl_file)

        self.assertEqual(
            result,
            [
                "//jstests/core:test1.js",
                "//jstests/core:test2.js",
                "//jstests/core:test3.js",
            ],
        )

    def test_extract_attribute_with_list_concatenation_and_identifiers(self):
        """Test extracting attribute with list concatenation including identifiers."""
        # Create a .bzl file with identifier definitions
        bzl_file = os.path.join(self.test_dir, "test_srcs.bzl")
        with open(bzl_file, "w") as f:
            f.write(
                """
base_tests = [
    "//jstests/base:test1.js",
    "//jstests/base:test2.js",
]

extra_tests = [
    "//jstests/extra:test3.js",
]
"""
            )

        block = """resmoke_suite_test(
    name = "test_suite",
    srcs = ["//jstests/core:custom.js"] + base_tests + extra_tests,
    shard_count = 2,
)"""
        identifier_to_bzl_file = {
            "base_tests": bzl_file,
            "extra_tests": bzl_file,
        }
        result = _extract_attribute(block, "srcs", identifier_to_bzl_file)

        self.assertEqual(
            result,
            [
                "//jstests/core:custom.js",
                "//jstests/base:test1.js",
                "//jstests/base:test2.js",
                "//jstests/extra:test3.js",
            ],
        )

    def test_resolve_identifier_in_same_build_file(self):
        """Test resolving identifier defined in the same BUILD.bazel file."""
        # Create a BUILD.bazel file with local identifier definition
        build_file = os.path.join(self.test_dir, "BUILD.bazel")
        with open(build_file, "w") as f:
            f.write(
                """
local_tests = [
    "//jstests/local:test1.js",
    "//jstests/local:test2.js",
]

resmoke_suite_test(
    name = "test_suite",
    srcs = local_tests,
    shard_count = 2,
)
"""
            )

        # The identifier maps to the BUILD.bazel file itself
        identifier_to_bzl_file = {"local_tests": build_file}
        result = _resolve_identifier_to_labels("local_tests", identifier_to_bzl_file)

        self.assertEqual(
            result,
            [
                "//jstests/local:test1.js",
                "//jstests/local:test2.js",
            ],
        )


if __name__ == "__main__":
    unittest.main()
