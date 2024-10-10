"""Unit tests for the selected_tests script."""

import os
import shutil
import sys
import unittest

from buildscripts import sbom_linter

TEST_DIR = os.path.join("buildscripts", "tests", "sbom_linter")


@unittest.skipIf(
    sys.platform == "darwin",
    reason="No need to run this unittest on macos since this is only needed for linux",
)
class TestSbom(unittest.TestCase):
    def setUp(self):
        sbom_linter.SKIP_FILE_CHECKING = True
        self.output_dir = os.path.join(TEST_DIR, "outputs")
        self.input_dir = os.path.join(TEST_DIR, "inputs")

        if not os.path.exists(self.output_dir):
            os.mkdir(self.output_dir)

    def tearDown(self):
        shutil.rmtree(self.output_dir)

    def assert_message_in_errors(self, error_manager: sbom_linter.ErrorManager, message: str):
        if not error_manager.find_message_in_errors(message):
            error_manager.print_errors()
            self.fail(f"Could not find error message matching: {message}")

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
        self.assert_message_in_errors(error_manager, sbom_linter.UNDEFINED_THIRD_PARTY_ERROR)

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
        self.assert_message_in_errors(
            error_manager, sbom_linter.COULD_NOT_FIND_OR_READ_SCRIPT_FILE_ERROR
        )

    def test_version_mismatch(self):
        test_file = os.path.join(self.input_dir, "sbom_version_mismatch.json")
        third_party_libs = {"librdkafka"}
        error_manager = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(error_manager, sbom_linter.VERSION_MISMATCH_ERROR)

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
