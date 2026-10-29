"""Unit tests for the buildscripts/tests/sbom_linter/sbom_linter.py script."""

import os
import shutil
import sys
import tempfile
import unittest

from buildscripts.tests.sbom_linter import sbom_linter

# Resolved relative to this test file's own directory (not the process cwd), since
# sbom_linter.main() changes the process cwd to BUILD_WORKSPACE_DIRECTORY when invoked
# via --check-metadata, and `bazel test` vs `bazel run` resolve relative paths differently.
TEST_DIR = os.path.dirname(os.path.abspath(__file__))


@unittest.skipIf(
    sys.platform == "darwin",
    reason="No need to run this unittest on macos since this is only needed for linux",
)
class TestSbom(unittest.TestCase):
    def setUp(self):
        sbom_linter.SKIP_FILE_CHECKING = True
        # A real temp directory (not cwd-relative) so it stays valid even if
        # sbom_linter.main() changes the process cwd during a test.
        self.output_dir = tempfile.mkdtemp(prefix="sbom_linter_test_outputs_")
        self.input_dir = os.path.join(TEST_DIR, "inputs")

    def tearDown(self):
        shutil.rmtree(self.output_dir)

    def assert_message_in_errors(self, error_manager: sbom_linter.ErrorManager, message: str):
        if not error_manager.find_message_in_errors(message):
            error_manager.print_errors()
            self.fail(f"Could not find error message matching: {message}")

    def assert_message_in_warnings(self, error_manager: sbom_linter.ErrorManager, message: str):
        if not error_manager.find_message_in_warnings(message):
            error_manager.print_errors()
            self.fail(f"Could not find warning message matching: {message}")

    def test_valid_sbom(self):
        test_file = os.path.join(self.input_dir, "valid_sbom.json")
        third_party_libs = {"librdkafka", "protobuf"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, True)
        if not error_manager.zero_error():
            error_manager.print_errors()
        self.assertTrue(error_manager.zero_error())

    def test_undefined_dep(self):
        test_file = os.path.join(self.input_dir, "valid_sbom.json")
        third_party_libs = {"librdkafka", "protobuf", "extra_dep"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_warnings(error_manager, sbom_linter.UNDEFINED_THIRD_PARTY_ERROR)
        self.assertTrue(error_manager.zero_error())

    def test_missing_purl_or_cpe(self):
        test_file = os.path.join(self.input_dir, "sbom_missing_purl.json")
        third_party_libs = {"librdkafka", "protobuf"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(error_manager, sbom_linter.MISSING_PURL_CPE_ERROR)

    def test_missing_evidence(self):
        test_file = os.path.join(self.input_dir, "sbom_missing_evidence.json")
        third_party_libs = {"librdkafka", "protobuf"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(error_manager, sbom_linter.MISSING_EVIDENCE_ERROR)

    def test_missing_team_responsible(self):
        test_file = os.path.join(self.input_dir, "sbom_missing_team.json")
        third_party_libs = {"librdkafka", "protobuf"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(error_manager, sbom_linter.MISSING_TEAM_ERROR)

    def test_format(self):
        test_file = os.path.join(self.input_dir, "sbom_invalid_format.json")
        output_file = os.path.join(self.output_dir, "new_valid_sbom1.json")
        third_party_libs = {"librdkafka", "protobuf"}
        error_manager = sbom_linter.lint_sbom(test_file, output_file, third_party_libs, True)
        self.assert_message_in_errors(error_manager, sbom_linter.FORMATTING_ERROR)

        error_manager = sbom_linter.lint_sbom(output_file, output_file, third_party_libs, False)
        self.assertTrue(error_manager.zero_error())

    def test_missing_version(self):
        test_file = os.path.join(self.input_dir, "sbom_missing_version.json")
        third_party_libs = {"librdkafka"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(
            error_manager, sbom_linter.MISSING_VERSION_IN_SBOM_COMPONENT_ERROR
        )

    def test_missing_version_in_import_file(self):
        test_file = os.path.join(self.input_dir, "sbom_script_missing_version.json")
        third_party_libs = {"librdkafka"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(
            error_manager, sbom_linter.MISSING_VERSION_IN_IMPORT_FILE_ERROR
        )

    def test_missing_import_file(self):
        test_file = os.path.join(self.input_dir, "sbom_script_file_missing.json")
        third_party_libs = {"librdkafka"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_warnings(
            error_manager, sbom_linter.COULD_NOT_FIND_OR_READ_SCRIPT_FILE_ERROR
        )
        self.assertTrue(error_manager.zero_error())

    def test_pedigree_version_match(self):
        test_file = os.path.join(self.input_dir, "sbom_pedigree_version_match.json")
        third_party_libs = {"kafka"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        if not error_manager.zero_error():
            error_manager.print_errors()
        self.assertTrue(error_manager.zero_error())

    def test_schema_match_failure(self):
        test_file = os.path.join(self.input_dir, "sbom_component_name_missing.json")
        third_party_libs = {"librdkafka"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(error_manager, sbom_linter.SCHEMA_MATCH_FAILURE)

    def test_component_empty_version(self):
        test_file = os.path.join(self.input_dir, "sbom_component_empty_version.json")
        third_party_libs = {"librdkafka"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(
            error_manager, sbom_linter.MISSING_VERSION_IN_SBOM_COMPONENT_ERROR
        )

    def test_missing_license(self):
        test_file = os.path.join(self.input_dir, "sbom_missing_license.json")
        third_party_libs = {"librdkafka"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(
            error_manager, sbom_linter.MISSING_LICENSE_IN_SBOM_COMPONENT_ERROR
        )

    def test_invalid_license_expression(self):
        test_file = os.path.join(self.input_dir, "sbom_invalid_license_expression.json")
        third_party_libs = {"librdkafka"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        # print(error_manager.errors)
        self.assert_message_in_errors(error_manager, "ExpressionInfo")

    def test_named_license(self):
        test_file = os.path.join(self.input_dir, "sbom_named_license.json")
        third_party_libs = {"murmurhash3"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        if not error_manager.zero_error():
            error_manager.print_errors()
        self.assertTrue(error_manager.zero_error())

    def test_licenseref_license(self):
        test_file = os.path.join(self.input_dir, "sbom_licenseref.json")
        third_party_libs = {"murmurhash3"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        if not error_manager.zero_error():
            error_manager.print_errors()
        self.assertTrue(error_manager.zero_error())

    def test_load_metadata_yaml_valid(self):
        """load_metadata parses a YAML flat-list into a CycloneDX BOM dict."""
        path = os.path.join(self.input_dir, "metadata_valid.cdx.yaml")
        bom = sbom_linter.load_metadata(path)
        self.assertIn("components", bom)
        self.assertIn("metadata", bom)
        self.assertEqual(bom["metadata"]["component"]["type"], "application")
        self.assertEqual(bom["components"][0]["name"], "test-dep")
        self.assertEqual(bom["dependencies"], [{"ref": "test-app", "dependsOn": ["test-dep"]}])

    def test_load_metadata_invalid_depends_on_type(self):
        """load_metadata exits 1 when dependsOn is a scalar string instead of a list."""
        path = os.path.join(self.input_dir, "metadata_invalid_depends_on.cdx.yaml")
        with self.assertRaises(SystemExit) as cm:
            sbom_linter.load_metadata(path)
        self.assertEqual(cm.exception.code, 1)

    def test_check_metadata_valid(self):
        """--check-metadata returns 0 and prints OK for a valid YAML metadata file."""
        path = os.path.join(self.input_dir, "metadata_valid.cdx.yaml")
        saved_argv = sys.argv
        try:
            sys.argv = ["buildscripts/tests/sbom_linter/sbom_linter.py", "--check-metadata", path]
            result = sbom_linter.main()
        finally:
            sys.argv = saved_argv
        self.assertEqual(result, 0)


if __name__ == "__main__":
    unittest.main()
