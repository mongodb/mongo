"""Unit tests for buildscripts/bazel_burn_in.py."""

import os
import unittest
from unittest.mock import mock_open, patch

import buildscripts.bazel_burn_in as under_test

NS = "buildscripts.bazel_burn_in"


def ns(relative_name):
    """Return a full name from a name relative to the test module's namespace."""
    return NS + "." + relative_name


# Mock data fixtures
MOCK_RESMOKE_CONFIG = {"test_kind": "js_test", "selector": {"roots": ["jstests/**/*.js"]}}

MOCK_EXCLUSIONS = {"selector": {"js_test": {"exclude_suites": [], "exclude_tests": []}}}

MOCK_BUILDOZER_RULE = """resmoke_suite_test(
    name = "core_config",
    resmoke_args = ["--log=info"],
    srcs = ["//jstests/core:all"],
    shard_count = 4,
)"""


class TestParseBazelTarget(unittest.TestCase):
    """Tests for parse_bazel_target function."""

    def test_parse_basic_target_with_colon(self):
        """Test parsing a basic target with colon and _config suffix."""
        result = under_test.parse_bazel_target("//buildscripts/resmokeconfig:core_config")
        self.assertEqual(
            (os.path.join("buildscripts", "resmokeconfig", "BUILD.bazel"), "core"), result
        )

    def test_parse_target_without_colon(self):
        """Test parsing a target without colon."""
        result = under_test.parse_bazel_target("//jstests/core")
        self.assertEqual((os.path.join("jstests", "core", "BUILD.bazel"), "core"), result)

    def test_parse_target_without_config_suffix(self):
        """Test parsing a target without _config suffix."""
        result = under_test.parse_bazel_target("//buildscripts:test_target")
        self.assertEqual((os.path.join("buildscripts", "BUILD.bazel"), "test_target"), result)

    def test_parse_target_with_double_slash_prefix(self):
        """Test parsing a target with // prefix."""
        result = under_test.parse_bazel_target("//path/to/package:target_name_config")
        self.assertEqual(
            (os.path.join("path", "to", "package", "BUILD.bazel"), "target_name"), result
        )

    def test_parse_target_without_double_slash(self):
        """Test parsing a target without // prefix."""
        result = under_test.parse_bazel_target("buildscripts:target")
        self.assertEqual((os.path.join("buildscripts", "BUILD.bazel"), "target"), result)

    def test_parse_target_with_nested_path(self):
        """Test parsing a target with deeply nested path."""
        result = under_test.parse_bazel_target("//a/b/c/d:target_config")
        self.assertEqual((os.path.join("a", "b", "c", "d", "BUILD.bazel"), "target"), result)

    def test_parse_target_edge_case_single_directory(self):
        """Test parsing a target with single directory."""
        result = under_test.parse_bazel_target("//jstests:test_config")
        self.assertEqual((os.path.join("jstests", "BUILD.bazel"), "test"), result)

    def test_parse_target_with_underscores_in_name(self):
        """Test parsing a target with underscores in name."""
        result = under_test.parse_bazel_target("//path:my_test_target_config")
        self.assertEqual((os.path.join("path", "BUILD.bazel"), "my_test_target"), result)

    def test_parse_target_config_in_middle_of_name(self):
        """Test that config in middle of name is NOT removed."""
        result = under_test.parse_bazel_target("//path:config_test_target")
        self.assertEqual((os.path.join("path", "BUILD.bazel"), "config_test_target"), result)


class TestCreateBurnInTarget(unittest.TestCase):
    """Unit tests for create_burn_in_target function."""

    @patch(ns("buildozer.bd_move"))
    @patch(ns("buildozer.bd_remove"))
    @patch(ns("buildozer.bd_set"))
    @patch(ns("buildozer.bd_print"))
    @patch("builtins.open", new_callable=mock_open)
    @patch(ns("parse_bazel_target"))
    def test_create_burn_in_target_basic(
        self, mock_parse, mock_open_file, mock_bd_print, mock_bd_set, mock_bd_remove, mock_bd_move
    ):
        """Test basic burn-in target creation."""
        # Setup
        target_original = "//jstests:core_config"
        target_burn_in = "//jstests:core_burn_in_find_js"
        test = "jstests/core/find.js"

        mock_parse.side_effect = [
            ("jstests/BUILD.bazel", "core"),
            ("jstests/BUILD.bazel", "core_burn_in_find_js"),
        ]

        mock_rule = 'resmoke_suite_test(\n    name = "core",\n    resmoke_args = [],\n)'
        mock_bd_print.side_effect = [mock_rule, "[]"]

        # Execute
        under_test.create_burn_in_target(target_original, target_burn_in, test)

        # Assert
        mock_parse.assert_any_call(target_original)
        mock_parse.assert_any_call(target_burn_in)

        mock_open_file.assert_called_once_with("jstests/BUILD.bazel", "a")

        mock_bd_set.assert_any_call([target_burn_in], "srcs", "//jstests/core:find.js")
        mock_bd_set.assert_any_call([target_burn_in], "shard_count", "1")

        # Verify resmoke_args includes repeat parameters
        resmoke_args_calls = [
            call for call in mock_bd_set.call_args_list if call[0][1] == "resmoke_args"
        ]
        self.assertEqual(len(resmoke_args_calls), 1)
        resmoke_args_value = resmoke_args_calls[0][0][2]

        self.assertIn("--repeatTestsMax=1000", resmoke_args_value)
        self.assertIn("--repeatTestsMin=2", resmoke_args_value)
        self.assertIn("--repeatTestsSecs=600.0", resmoke_args_value)

    @patch(ns("buildozer.bd_move"))
    @patch(ns("buildozer.bd_remove"))
    @patch(ns("buildozer.bd_set"))
    @patch(ns("buildozer.bd_print"))
    @patch("builtins.open", new_callable=mock_open)
    @patch(ns("parse_bazel_target"))
    def test_create_burn_in_target_with_existing_resmoke_args(
        self, mock_parse, mock_open_file, mock_bd_print, mock_bd_set, mock_bd_remove, mock_bd_move
    ):
        """Test burn-in target creation preserves existing resmoke args."""
        # Setup
        target_original = "//jstests:core_config"
        target_burn_in = "//jstests:core_burn_in_find_js"
        test = "jstests/core/find.js"

        mock_parse.side_effect = [
            ("jstests/BUILD.bazel", "core"),
            ("jstests/BUILD.bazel", "core_burn_in_find_js"),
        ]

        mock_bd_print.return_value = (
            'resmoke_suite_test(\n    name = "core",\n'
            '    resmoke_args = ["--log=debug", "--storageEngine=wiredTiger"],\n)'
        )

        # Execute
        under_test.create_burn_in_target(target_original, target_burn_in, test)

        # Assert - verify existing args preserved and new args added
        resmoke_args_calls = [
            call for call in mock_bd_set.call_args_list if call[0][1] == "resmoke_args"
        ]
        resmoke_args_value = resmoke_args_calls[0][0][2]

        self.assertIn("--log=debug", resmoke_args_value)
        self.assertIn("--storageEngine=wiredTiger", resmoke_args_value)
        self.assertIn("--repeatTestsMax=1000", resmoke_args_value)

    @patch(ns("buildozer.bd_move"))
    @patch(ns("buildozer.bd_remove"))
    @patch(ns("buildozer.bd_set"))
    @patch(ns("buildozer.bd_print"))
    @patch("builtins.open", new_callable=mock_open)
    @patch(ns("parse_bazel_target"))
    def test_create_burn_in_target_with_missing_resmoke_args(
        self, mock_parse, mock_open_file, mock_bd_print, mock_bd_set, mock_bd_remove, mock_bd_move
    ):
        """Test burn-in target handles missing resmoke_args."""
        # Setup
        target_original = "//jstests:core_config"
        target_burn_in = "//jstests:core_burn_in_find_js"
        test = "jstests/core/find.js"

        mock_parse.side_effect = [
            ("jstests/BUILD.bazel", "core"),
            ("jstests/BUILD.bazel", "core_burn_in_find_js"),
        ]

        mock_bd_print.side_effect = ['resmoke_suite_test(name = "core")', "(missing)"]

        # Execute
        under_test.create_burn_in_target(target_original, target_burn_in, test)

        # Assert - verify (missing) is not in the args
        resmoke_args_calls = [
            call for call in mock_bd_set.call_args_list if call[0][1] == "resmoke_args"
        ]
        resmoke_args_value = resmoke_args_calls[0][0][2]

        self.assertNotIn("(missing)", resmoke_args_value)
        self.assertIn("--repeatTestsMax=1000", resmoke_args_value)

    @patch(ns("buildozer.bd_move"))
    @patch(ns("buildozer.bd_remove"))
    @patch(ns("buildozer.bd_set"))
    @patch(ns("buildozer.bd_print"))
    @patch("builtins.open", new_callable=mock_open)
    @patch(ns("parse_bazel_target"))
    def test_create_burn_in_target_sets_correct_attributes(
        self, mock_parse, mock_open_file, mock_bd_print, mock_bd_set, mock_bd_remove, mock_bd_move
    ):
        """Test that bd_set is called with correct attributes."""
        # Setup
        target_original = "//jstests:core_config"
        target_burn_in = "//jstests:core_burn_in_find_js"
        test = "jstests/core/find.js"

        mock_parse.side_effect = [
            ("jstests/BUILD.bazel", "core"),
            ("jstests/BUILD.bazel", "core_burn_in_find_js"),
        ]

        mock_bd_print.side_effect = ['resmoke_suite_test(name = "core")', "[]"]

        # Execute
        under_test.create_burn_in_target(target_original, target_burn_in, test)

        # Assert - bd_move called to move existing srcs to data
        mock_bd_move.assert_called_once_with([target_burn_in], "srcs", "data")

        # Assert - bd_remove called to remove the test label from data, if present
        mock_bd_remove.assert_called_once_with([target_burn_in], "data", ["//jstests/core:find.js"])

        # Assert - bd_set called 3 times for srcs, shard_count, and resmoke_args
        self.assertEqual(mock_bd_set.call_count, 3)
        mock_bd_set.assert_any_call([target_burn_in], "srcs", "//jstests/core:find.js")
        mock_bd_set.assert_any_call([target_burn_in], "shard_count", "1")


class TestGetTargetsWithTag(unittest.TestCase):
    """Tests for get_targets_with_tag function."""

    def setUp(self):
        # Clear the @cache of get_targets_with_tag between tests
        under_test.get_targets_with_tag.cache_clear()

    @patch(ns("subprocess.run"))
    def test_query_excludes_incompatible_with_bazel_remote_test(self, mock_run):
        """Test that the bazel query subtracts incompatible_with_bazel_remote_test targets."""
        mock_run.return_value.stdout = "//jstests/suites/foo:bar\n"

        under_test.get_targets_with_tag("ci-release-critical")

        query_arg = mock_run.call_args[0][0]
        query_str = " ".join(query_arg)
        # The query passed to bazel should contain a set difference excluding
        # targets tagged with incompatible_with_bazel_remote_test
        self.assertIn("incompatible_with_bazel_remote_test", query_str)


class TestGetTargetsMatchingTagFilter(unittest.TestCase):
    """Tests for get_targets_matching_tag_filter function."""

    TAGS_TO_TARGETS = {
        "ci-development-critical": ["//s:dev"],
        "ci-release-critical": ["//s:rel", "//s:mitigated"],
        "suggested_excluding_required__for_devprod_mitigation_only": ["//s:mitigated"],
        "requires_all_feature_flags": [],
        "requires_compile_variant": [],
        "incompatible_development_variant": ["//s:dev"],
    }

    def setUp(self):
        patcher = patch(ns("get_targets_with_tag"), side_effect=self.TAGS_TO_TARGETS.__getitem__)
        self.mock_get_targets_with_tag = patcher.start()
        self.addCleanup(patcher.stop)

    def test_union_of_positive_tags(self):
        self.assertEqual(
            under_test.get_targets_matching_tag_filter(
                "ci-development-critical,ci-release-critical"
            ),
            {"//s:dev", "//s:rel", "//s:mitigated"},
        )

    def test_negated_tags_are_excluded(self):
        self.assertEqual(
            under_test.get_targets_matching_tag_filter(
                "ci-development-critical,ci-release-critical,"
                "-suggested_excluding_required__for_devprod_mitigation_only,"
                "-requires_all_feature_flags,-requires_compile_variant,"
                "-incompatible_development_variant"
            ),
            {"//s:rel"},
        )

    def test_negations_query_the_tag_without_the_dash(self):
        under_test.get_targets_matching_tag_filter("ci-release-critical,-requires_compile_variant")

        queried = [call.args[0] for call in self.mock_get_targets_with_tag.call_args_list]
        self.assertEqual(queried, ["ci-release-critical", "requires_compile_variant"])

    def test_blank_entries_are_ignored(self):
        self.assertEqual(
            under_test.get_targets_matching_tag_filter(" ci-release-critical , "),
            {"//s:rel", "//s:mitigated"},
        )


if __name__ == "__main__":
    unittest.main()
