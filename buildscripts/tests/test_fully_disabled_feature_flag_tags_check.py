"""Unit tests for fully_disabled_feature_flag_tags_check.py."""

import os
import shutil
import subprocess
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from unittest.mock import Mock, call, patch

from buildscripts import fully_disabled_feature_flag_tags_check


class TestGetNewFullyDisabledTaggedTests(unittest.TestCase):
    def test_new_test_with_fully_disabled_tag_reported(self):
        base_tags = {}
        patch_tags = {"jstests/core/new_test.js": ["featureFlagToaster"]}
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags, patch_tags, ["featureFlagToaster"]
        )
        self.assertEqual(["jstests/core/new_test.js: featureFlagToaster"], result)

    def test_new_test_with_multiple_fully_disabled_tags_reported(self):
        base_tags = {}
        patch_tags = {"jstests/core/new_test.js": ["featureFlagToaster", "featureFlagSpoon"]}
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags, patch_tags, ["featureFlagToaster", "featureFlagSpoon"]
        )
        self.assertEqual(["jstests/core/new_test.js: featureFlagToaster, featureFlagSpoon"], result)

    def test_test_not_reported_when_tag_not_fully_disabled(self):
        base_tags = {}
        patch_tags = {"jstests/core/new_test.js": ["featureFlagToaster"]}
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags, patch_tags, ["featureFlagSpoon"]
        )
        self.assertEqual([], result)

    def test_existing_test_tag_not_reported_when_already_tagged_in_base(self):
        base_tags = {"jstests/core/existing_test.js": ["featureFlagToaster"]}
        patch_tags = {"jstests/core/existing_test.js": ["featureFlagToaster", "featureFlagSpoon"]}
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags, patch_tags, ["featureFlagToaster"]
        )
        self.assertEqual([], result)

    def test_existing_test_reported_when_tag_added_in_patch(self):
        base_tags = {"jstests/core/existing_test.js": []}
        patch_tags = {"jstests/core/existing_test.js": ["featureFlagToaster"]}
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags, patch_tags, ["featureFlagToaster"]
        )
        self.assertEqual(["jstests/core/existing_test.js: featureFlagToaster"], result)

    def test_test_removed_in_patch_not_reported(self):
        base_tags = {"jstests/core/removed_test.js": ["featureFlagToaster"]}
        patch_tags = {}
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags, patch_tags, ["featureFlagToaster"]
        )
        self.assertEqual([], result)

    def test_violations_sorted_by_test_path(self):
        base_tags = {}
        patch_tags = {
            "jstests/z.js": ["featureFlagToaster"],
            "jstests/a.js": ["featureFlagToaster"],
        }
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags, patch_tags, ["featureFlagToaster"]
        )
        self.assertEqual(
            ["jstests/a.js: featureFlagToaster", "jstests/z.js: featureFlagToaster"], result
        )

    def test_renamed_test_tag_not_reported_when_already_tagged_in_base(self):
        base_tags = {"jstests/core/old_test.js": ["featureFlagToaster"]}
        patch_tags = {"jstests/core/new_test.js": ["featureFlagToaster"]}
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags,
            patch_tags,
            ["featureFlagToaster"],
            {"jstests/core/old_test.js": "jstests/core/new_test.js"},
        )
        self.assertEqual([], result)

    def test_renamed_test_reported_when_tag_added_in_patch(self):
        base_tags = {"jstests/core/old_test.js": []}
        patch_tags = {"jstests/core/new_test.js": ["featureFlagToaster"]}
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags,
            patch_tags,
            ["featureFlagToaster"],
            {"jstests/core/old_test.js": "jstests/core/new_test.js"},
        )
        self.assertEqual(["jstests/core/new_test.js: featureFlagToaster"], result)

    def test_renamed_test_tag_not_reported_when_target_path_also_existed_in_base(self):
        # git only reports a rename when the target path is newly added, but a caller that
        # supplies both must still get the grandfathering the rename mapping asks for.
        base_tags = {
            "jstests/core/old_test.js": ["featureFlagToaster"],
            "jstests/core/new_test.js": [],
        }
        patch_tags = {"jstests/core/new_test.js": ["featureFlagToaster"]}
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags,
            patch_tags,
            ["featureFlagToaster"],
            {"jstests/core/old_test.js": "jstests/core/new_test.js"},
        )
        self.assertEqual([], result)

    def test_copied_test_reported_as_new_test(self):
        base_tags = {"jstests/core/orig_test.js": ["featureFlagToaster"]}
        patch_tags = {
            "jstests/core/orig_test.js": ["featureFlagToaster"],
            "jstests/core/copy_test.js": ["featureFlagToaster"],
        }
        result = fully_disabled_feature_flag_tags_check.get_new_fully_disabled_tagged_tests(
            base_tags, patch_tags, ["featureFlagToaster"]
        )
        self.assertEqual(["jstests/core/copy_test.js: featureFlagToaster"], result)


class TestGetRenamedTests(unittest.TestCase):
    def get_renamed_tests(self, diff):
        """Parse the given diff text via a temporary diff file."""
        with tempfile.NamedTemporaryFile("w", suffix=".diff", delete=False) as fh:
            fh.write(diff)
            pathname = fh.name
        self.addCleanup(os.remove, pathname)
        return fully_disabled_feature_flag_tags_check.get_renamed_tests(pathname)

    def test_rename_parsed(self):
        self.assertEqual(
            {"jstests/core/old_test.js": "jstests/core/new_test.js"},
            self.get_renamed_tests(
                "diff --git a/jstests/core/old_test.js b/jstests/core/new_test.js\n"
                "similarity index 95%\n"
                "rename from jstests/core/old_test.js\n"
                "rename to jstests/core/new_test.js\n"
                "--- a/jstests/core/old_test.js\n"
                "+++ b/jstests/core/new_test.js\n"
            ),
        )

    def test_multiple_renames_parsed(self):
        self.assertEqual(
            {
                "jstests/core/a.js": "jstests/core/b.js",
                "jstests/core/c.js": "jstests/core/d.js",
            },
            self.get_renamed_tests(
                "diff --git a/jstests/core/a.js b/jstests/core/b.js\n"
                "rename from jstests/core/a.js\n"
                "rename to jstests/core/b.js\n"
                "diff --git a/jstests/core/c.js b/jstests/core/d.js\n"
                "rename from jstests/core/c.js\n"
                "rename to jstests/core/d.js\n"
            ),
        )

    def test_copy_not_parsed_as_rename(self):
        self.assertEqual(
            {},
            self.get_renamed_tests(
                "diff --git a/jstests/core/orig_test.js b/jstests/core/copy_test.js\n"
                "copy from jstests/core/orig_test.js\n"
                "copy to jstests/core/copy_test.js\n"
            ),
        )

    def test_added_and_deleted_files_not_parsed_as_renames(self):
        self.assertEqual(
            {},
            self.get_renamed_tests(
                "diff --git a/jstests/core/deleted.js b/jstests/core/deleted.js\n"
                "deleted file mode 100644\n"
                "diff --git a/jstests/core/added.js b/jstests/core/added.js\n"
                "new file mode 100644\n"
            ),
        )

    def test_incomplete_rename_ignored(self):
        self.assertEqual(
            {},
            self.get_renamed_tests(
                "diff --git a/jstests/core/old_test.js b/jstests/core/old_test.js\n"
                "rename from jstests/core/old_test.js\n"
                "diff --git a/jstests/core/other.js b/jstests/core/other.js\n"
            ),
        )


class TestGetEvergreenEnabledFeatureFlags(unittest.TestCase):
    def get_enabled_flags(self, files):
        """Write the given {relative pathname: contents} files under a temp dir and scan it."""
        temp_dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, temp_dir, ignore_errors=True)
        for rel_pathname, contents in files.items():
            pathname = os.path.join(temp_dir, rel_pathname)
            os.makedirs(os.path.dirname(pathname), exist_ok=True)
            with open(pathname, "w", encoding="utf8") as fh:
                fh.write(contents)
        return fully_disabled_feature_flag_tags_check.get_evergreen_enabled_feature_flags(
            [os.path.join(temp_dir, rel_pathname) for rel_pathname in files]
        )

    def test_flags_enabled_with_all_argument_syntaxes_found(self):
        self.assertEqual(
            {"featureFlagToaster", "featureFlagSpoon", "featureFlagKnife", "featureFlagFork"},
            self.get_enabled_flags(
                {
                    "variants/test_dev.yml": """\
                        expansions:
                          test_flags: >-
                            --additionalFeatureFlags=featureFlagToaster
                            --additionalFeatureFlags=featureFlagSpoon,featureFlagKnife
                            --additionalFeatureFlags "featureFlagFork"
                            --additionalFeatureFlags featureFlagToaster
                    """,
                    "variants/streams/streams_dev.yml": """\
                        expansions:
                          test_flags: --additionalFeatureFlags=featureFlagToaster --suite=no_passthrough
                    """,
                }
            ),
        )

    def test_trailing_argument_does_not_capture_the_next_line(self):
        self.assertEqual(
            set(),
            self.get_enabled_flags(
                {
                    "variants/test_dev.yml": """\
                        expansions:
                          test_flags: --additionalFeatureFlags
                        suite: no_passthrough
                    """
                }
            ),
        )

    def test_additional_feature_flags_file_argument_not_matched(self):
        self.assertEqual(
            set(),
            self.get_enabled_flags(
                {"variants/test_dev.yml": "--additionalFeatureFlagsFile=feature_flags.txt\n"}
            ),
        )

    def test_commented_out_example_line_ignored(self):
        # A commented-out --additionalFeatureFlags example is not active configuration and must
        # not exempt its flag from the check.
        self.assertEqual(
            set(),
            self.get_enabled_flags(
                {
                    "variants/test_dev.yml": (
                        "# Run with --additionalFeatureFlags=featureFlagToaster to test it.\n"
                        "  # --additionalFeatureFlags featureFlagSpoon\n"
                    )
                }
            ),
        )

    def test_flag_in_trailing_comment_ignored(self):
        # A trailing YAML comment is not active configuration either.
        self.assertEqual(
            set(),
            self.get_enabled_flags(
                {
                    "variants/test_dev.yml": (
                        "description: value # --additionalFeatureFlags=featureFlagToaster\n"
                    )
                }
            ),
        )

    def test_flag_in_description_ignored(self):
        self.assertEqual(
            set(),
            self.get_enabled_flags(
                {
                    "variants/test_dev.yml": (
                        'description: "Run locally with '
                        '--additionalFeatureFlags=featureFlagToaster"\n'
                    )
                }
            ),
        )

    def test_hash_inside_quoted_string_is_not_a_comment(self):
        # '#' inside a quoted string is data, so parsing must retain the active flag that follows
        # it in the test arguments.
        self.assertEqual(
            {"featureFlagToaster"},
            self.get_enabled_flags(
                {
                    "variants/test_dev.yml": (
                        "expansions:\n"
                        '  test_flags: "see # note --additionalFeatureFlags=featureFlagToaster"\n'
                    )
                }
            ),
        )

    def test_active_line_with_trailing_comment_still_matched(self):
        self.assertEqual(
            {"featureFlagToaster"},
            self.get_enabled_flags(
                {
                    "variants/test_dev.yml": """\
                        expansions:
                          test_flags: --additionalFeatureFlags=featureFlagToaster # nightly
                    """
                }
            ),
        )

    def test_only_active_flags_found_when_commented_and_active_lines_mixed(self):
        self.assertEqual(
            {"featureFlagToaster"},
            self.get_enabled_flags(
                {
                    "variants/test_dev.yml": """\
                        # --additionalFeatureFlags=featureFlagSpoon
                        expansions:
                          test_flags: --additionalFeatureFlags=featureFlagToaster
                        # --additionalFeatureFlags=featureFlagKnife
                    """
                }
            ),
        )

    def test_non_yml_files_ignored(self):
        self.assertEqual(
            set(),
            self.get_enabled_flags({"variants/test_dev.json": "--additionalFeatureFlags=flagA\n"}),
        )

    def test_missing_config_file_yields_no_flags(self):
        self.assertEqual(
            set(),
            fully_disabled_feature_flag_tags_check.get_evergreen_enabled_feature_flags(
                [
                    os.path.join(
                        tempfile.gettempdir(), "no_such_file_for_feature_flag_tags_check.yml"
                    )
                ]
            ),
        )

    def test_only_active_included_configs_are_scanned(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root_config = os.path.join(temp_dir, "evergreen.yml")
            included_config = os.path.join(temp_dir, "included.yml")
            inactive_config = os.path.join(temp_dir, "inactive.yml")
            with open(root_config, "w", encoding="utf8") as fh:
                fh.write("include:\n  - filename: " + included_config + "\n")
            with open(included_config, "w", encoding="utf8") as fh:
                fh.write("expansions:\n  test_flags: --additionalFeatureFlags=featureFlagActive\n")
            with open(inactive_config, "w", encoding="utf8") as fh:
                fh.write(
                    "expansions:\n  test_flags: --additionalFeatureFlags=featureFlagInactive\n"
                )

            active_configs = (
                fully_disabled_feature_flag_tags_check.get_active_evergreen_config_files(
                    root_config
                )
            )

            self.assertEqual([root_config, included_config], active_configs)
            self.assertEqual(
                {"featureFlagActive"},
                fully_disabled_feature_flag_tags_check.get_evergreen_enabled_feature_flags(
                    active_configs
                ),
            )


class TestLoadFullyDisabledFlags(unittest.TestCase):
    def test_load_flags(self):
        with tempfile.NamedTemporaryFile("w", suffix=".yml", delete=False) as fh:
            fh.write("# A comment.\n- featureFlagToaster\n- featureFlagSpoon\n")
            pathname = fh.name
        try:
            result = fully_disabled_feature_flag_tags_check.load_fully_disabled_flags(pathname)
        finally:
            os.remove(pathname)
        self.assertEqual(["featureFlagToaster", "featureFlagSpoon"], result)

    def test_load_flags_empty_file(self):
        with tempfile.NamedTemporaryFile("w", suffix=".yml", delete=False) as fh:
            fh.write("")
            pathname = fh.name
        try:
            result = fully_disabled_feature_flag_tags_check.load_fully_disabled_flags(pathname)
        finally:
            os.remove(pathname)
        self.assertEqual([], result)


class TestDiscoverJstestTags(unittest.TestCase):
    ENTERPRISE_PATH = "src/mongo/db/modules/enterprise"

    def discover(self, enterprise_path=None, empty_roots=()):
        """Run discovery with the roots in empty_roots matching no file.

        Returns the roots that were handed to the selector. Globbing is stubbed so the result
        does not depend on which module directories this checkout happens to have.
        """
        with (
            patch.object(
                fully_disabled_feature_flag_tags_check.selector,
                "filter_tests",
                return_value=([], []),
            ) as filter_tests,
            patch.object(
                fully_disabled_feature_flag_tags_check.globstar,
                "glob",
                side_effect=lambda root: [] if root in empty_roots else [f"{root}/a_test.js"],
            ),
        ):
            self.assertEqual(
                {}, fully_disabled_feature_flag_tags_check.discover_jstest_tags(enterprise_path)
            )
        return filter_tests.call_args.args[1]["roots"]

    def test_module_jstest_roots_covered(self):
        # The Atlas module's jstests are vendored into the repository, so unlike the enterprise
        # module's jstests they are always discovered, without needing a module path argument.
        self.assertIn(
            "src/mongo/db/modules/atlas/jstests/**/*.js",
            fully_disabled_feature_flag_tags_check.JSTEST_ROOTS,
        )

    def test_discovers_main_and_enterprise_jstests(self):
        self.assertEqual(fully_disabled_feature_flag_tags_check.JSTEST_ROOTS, self.discover())

    def test_discover_with_enterprise_path_includes_enterprise_root(self):
        enterprise_root = fully_disabled_feature_flag_tags_check.ENTERPRISE_JSTEST_ROOT.format(
            enterprise_path=self.ENTERPRISE_PATH
        )
        self.assertEqual(
            fully_disabled_feature_flag_tags_check.JSTEST_ROOTS + [enterprise_root],
            self.discover(self.ENTERPRISE_PATH),
        )

    def test_root_of_absent_module_is_dropped(self):
        # selector.filter_tests raises on a root that matches nothing, so a module missing from
        # the checkout must narrow discovery rather than fail the whole check.
        atlas_root = "src/mongo/db/modules/atlas/jstests/**/*.js"
        enterprise_root = fully_disabled_feature_flag_tags_check.ENTERPRISE_JSTEST_ROOT.format(
            enterprise_path=self.ENTERPRISE_PATH
        )
        self.assertEqual(
            ["jstests/**/*.js"],
            self.discover(self.ENTERPRISE_PATH, empty_roots=(atlas_root, enterprise_root)),
        )

    def test_all_roots_absent_yields_no_tests(self):
        self.assertEqual(
            [],
            self.discover(empty_roots=tuple(fully_disabled_feature_flag_tags_check.JSTEST_ROOTS)),
        )


class TestRunGitCmd(unittest.TestCase):
    def test_raises_with_git_output_when_git_command_fails(self):
        proc = subprocess.CompletedProcess(
            ["git", "apply"], 1, stdout="", stderr="error: unrecognized input"
        )
        with patch.object(
            fully_disabled_feature_flag_tags_check.subprocess, "run", return_value=proc
        ):
            with self.assertRaisesRegex(RuntimeError, "unrecognized input"):
                fully_disabled_feature_flag_tags_check._run_git_cmd(["apply", "patch.diff"])

    def test_returns_completed_process_when_git_command_succeeds(self):
        proc = subprocess.CompletedProcess(["git", "apply"], 0, stdout="", stderr="")
        with patch.object(
            fully_disabled_feature_flag_tags_check.subprocess, "run", return_value=proc
        ):
            fully_disabled_feature_flag_tags_check._run_git_cmd(["apply", "patch.diff"])


class TestMain(unittest.TestCase):
    """Tests for the orchestration in main(): revert -> discover base tags -> restore."""

    DEFAULT_DIFF = "diff --git a/f.txt b/f.txt\n"

    def make_diff_file(self, diff=None):
        """Create a non-empty diff file, since main() checks that the diff exists and is not empty."""
        if diff is None:
            diff = self.DEFAULT_DIFF
        with tempfile.NamedTemporaryFile("w", delete=False) as fh:
            fh.write(diff)
            pathname = fh.name
        self.addCleanup(os.remove, pathname)
        return pathname

    def run_main(self, base_tags, patch_tags, evergreen_enabled_flags=frozenset(), diff=None):
        """Run main() with discovery, git commands and the Evergreen flag scan mocked.

        Returns the exit code, the captured output, the discovery mock, the git command mock and
        the diff file name.
        """
        diff_file_name = self.make_diff_file(diff)
        with (
            patch.object(
                fully_disabled_feature_flag_tags_check,
                "load_fully_disabled_flags",
                return_value=["featureFlagToaster"],
            ),
            patch.object(
                fully_disabled_feature_flag_tags_check,
                "get_evergreen_enabled_feature_flags",
                return_value=set(evergreen_enabled_flags),
            ),
            patch.object(
                fully_disabled_feature_flag_tags_check,
                "discover_jstest_tags",
                side_effect=[patch_tags, base_tags],
            ) as discover,
            patch.object(fully_disabled_feature_flag_tags_check, "_run_git_cmd") as git_cmd,
            redirect_stdout(StringIO()) as output,
        ):
            with self.assertRaises(SystemExit) as ctx:
                fully_disabled_feature_flag_tags_check.main(diff_file_name)
        return ctx.exception.code, output, discover, git_cmd, diff_file_name

    def test_exits_1_and_reports_newly_tagged_tests(self):
        exit_code, output, _, _, _ = self.run_main(
            base_tags={}, patch_tags={"jstests/core/new_test.js": ["featureFlagToaster"]}
        )
        self.assertEqual(1, exit_code)
        self.assertIn("jstests/core/new_test.js: featureFlagToaster", output.getvalue())
        self.assertIn("Remove the tag from these tests.", output.getvalue())

    def test_exits_0_when_no_newly_tagged_tests(self):
        tags = {"jstests/core/test.js": ["featureFlagToaster"]}
        exit_code, output, _, _, _ = self.run_main(base_tags=tags, patch_tags=tags)
        self.assertEqual(0, exit_code)
        self.assertEqual("", output.getvalue())

    def test_exits_0_when_flag_enabled_on_evergreen_variant(self):
        exit_code, output, _, _, _ = self.run_main(
            base_tags={},
            patch_tags={"jstests/core/new_test.js": ["featureFlagToaster"]},
            evergreen_enabled_flags={"featureFlagToaster"},
        )
        self.assertEqual(0, exit_code)
        self.assertEqual("", output.getvalue())

    def test_exits_0_when_renamed_test_keeps_its_tag(self):
        rename_diff = (
            "diff --git a/jstests/core/old_test.js b/jstests/core/new_test.js\n"
            "rename from jstests/core/old_test.js\n"
            "rename to jstests/core/new_test.js\n"
        )
        exit_code, output, _, _, _ = self.run_main(
            base_tags={"jstests/core/old_test.js": ["featureFlagToaster"]},
            patch_tags={"jstests/core/new_test.js": ["featureFlagToaster"]},
            diff=rename_diff,
        )
        self.assertEqual(0, exit_code)
        self.assertEqual("", output.getvalue())

    def test_exits_1_when_renamed_test_adds_a_tag(self):
        rename_diff = (
            "diff --git a/jstests/core/old_test.js b/jstests/core/new_test.js\n"
            "rename from jstests/core/old_test.js\n"
            "rename to jstests/core/new_test.js\n"
        )
        exit_code, output, _, _, _ = self.run_main(
            base_tags={"jstests/core/old_test.js": []},
            patch_tags={"jstests/core/new_test.js": ["featureFlagToaster"]},
            diff=rename_diff,
        )
        self.assertEqual(1, exit_code)
        self.assertIn("jstests/core/new_test.js: featureFlagToaster", output.getvalue())

    def test_reverts_patch_before_base_discovery_and_restores_after(self):
        diff_file_name = self.make_diff_file()
        with (
            patch.object(
                fully_disabled_feature_flag_tags_check,
                "load_fully_disabled_flags",
                return_value=["featureFlagToaster"],
            ),
            patch.object(
                fully_disabled_feature_flag_tags_check,
                "get_evergreen_enabled_feature_flags",
                return_value=set(),
            ),
            patch.object(
                fully_disabled_feature_flag_tags_check,
                "discover_jstest_tags",
                side_effect=[{}, {}],
            ) as discover,
            patch.object(fully_disabled_feature_flag_tags_check, "_run_git_cmd") as git_cmd,
        ):
            calls = Mock()
            calls.attach_mock(discover, "discover")
            calls.attach_mock(git_cmd, "git")
            with self.assertRaises(SystemExit):
                fully_disabled_feature_flag_tags_check.main(diff_file_name)

        self.assertEqual(
            [
                call.discover(None),
                call.git(["apply", "-R", diff_file_name]),
                call.discover(None),
                call.git(["apply", diff_file_name]),
            ],
            calls.mock_calls,
        )

    def test_restores_patch_when_base_discovery_raises(self):
        diff_file_name = self.make_diff_file()
        with (
            patch.object(
                fully_disabled_feature_flag_tags_check,
                "load_fully_disabled_flags",
                return_value=["featureFlagToaster"],
            ),
            patch.object(
                fully_disabled_feature_flag_tags_check,
                "get_evergreen_enabled_feature_flags",
                return_value=set(),
            ),
            patch.object(
                fully_disabled_feature_flag_tags_check,
                "discover_jstest_tags",
                side_effect=[{}, RuntimeError("discovery failed")],
            ),
            patch.object(fully_disabled_feature_flag_tags_check, "_run_git_cmd") as git_cmd,
        ):
            with self.assertRaisesRegex(RuntimeError, "discovery failed"):
                fully_disabled_feature_flag_tags_check.main(diff_file_name)

        self.assertEqual(
            [
                call(["apply", "-R", diff_file_name]),
                call(["apply", diff_file_name]),
            ],
            git_cmd.call_args_list,
        )

    def test_empty_diff_exits_cleanly_without_touching_the_tree(self):
        with tempfile.NamedTemporaryFile("w", delete=False) as fh:
            pathname = fh.name
        try:
            with (
                patch.object(
                    fully_disabled_feature_flag_tags_check, "discover_jstest_tags"
                ) as discover,
                patch.object(fully_disabled_feature_flag_tags_check, "_run_git_cmd") as git_cmd,
            ):
                with self.assertRaises(SystemExit) as ctx:
                    fully_disabled_feature_flag_tags_check.main(pathname)
        finally:
            os.remove(pathname)
        self.assertEqual(0, ctx.exception.code)
        discover.assert_not_called()
        git_cmd.assert_not_called()


if __name__ == "__main__":
    unittest.main()
