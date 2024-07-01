"""Unit tests for the selected_tests script."""

import os
import shutil
import unittest

from buildscripts import sbom_linter

TEST_DIR = os.path.join("buildscripts", "tests", "sbom_linter")


class TestSbom(unittest.TestCase):
    def setUp(self):
        sbom_linter.SKIP_FILE_CHECKING = True
        self.output_dir = os.path.join(TEST_DIR, "outputs")
        self.input_dir = os.path.join(TEST_DIR, "inputs")

        if not os.path.exists(self.output_dir):
            os.mkdir(self.output_dir)

    def tearDown(self):
        shutil.rmtree(self.output_dir)

    def assert_message_in_errors(self, errors: list, message: str):
        contained_message = False
        for error in errors:
            if message in error:
                contained_message = True
                break
        if not contained_message:
            self.fail(f"Could not find error message matching: {message}")

    def test_valid_sbom(self):
        test_file = os.path.join(self.input_dir, "valid_sbom.json")
        third_party_libs = {"librdkafka", "protobuf"}
        errors = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, True)
        self.assertFalse(errors)

    def test_undefined_dep(self):
        test_file = os.path.join(self.input_dir, "valid_sbom.json")
        third_party_libs = {"librdkafka", "protobuf", "extra_dep"}
        errors = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(errors, sbom_linter.UNDEFINED_THIRD_PARTY_ERROR)

    def test_missing_purl_or_cpe(self):
        test_file = os.path.join(self.input_dir, "sbom_missing_purl.json")
        third_party_libs = {"librdkafka", "protobuf"}
        errors = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(errors, sbom_linter.MISSING_PURL_CPE_ERROR)

    def test_missing_evidence(self):
        test_file = os.path.join(self.input_dir, "sbom_missing_evidence.json")
        third_party_libs = {"librdkafka", "protobuf"}
        errors = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(errors, sbom_linter.MISSING_EVIDENCE_ERROR)

    def test_missing_team_responsible(self):
        test_file = os.path.join(self.input_dir, "sbom_missing_team.json")
        third_party_libs = {"librdkafka", "protobuf"}
        errors = sbom_linter.lint_sbom(test_file, test_file, third_party_libs, False)
        self.assert_message_in_errors(errors, sbom_linter.MISSING_TEAM_ERROR)

    def test_format(self):
        test_file = os.path.join(self.input_dir, "sbom_invalid_format.json")
        output_file = os.path.join(self.output_dir, "new_valid_sbom1.json")
        third_party_libs = {"librdkafka", "protobuf"}
        errors = sbom_linter.lint_sbom(test_file, output_file, third_party_libs, True)
        self.assert_message_in_errors(errors, sbom_linter.FORMATTING_ERROR)

        errors = sbom_linter.lint_sbom(output_file, output_file, third_party_libs, False)
        self.assertFalse(errors)
