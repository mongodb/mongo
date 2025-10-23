import sys
import unittest
from contextlib import redirect_stderr
from io import StringIO

sys.path.append(".")

from bazel.wrapper_hook.plus_interface import (
    BinAndSourceIncompatible,
    DuplicateSourceNames,
    test_runner_interface,
)


def validate_first_suggestion(stderr_output: str, expected_suggestion: str):
    assert (
        "Did you mean one of these?" in stderr_output
    ), f"Expected 'Did you mean one of these?' in stderr, got: {stderr_output}"

    suggestion_section = stderr_output.split("Did you mean one of these?")[1]
    first_suggestion_line = [
        line.strip()
        for line in suggestion_section.split("\n")
        if line.strip() and line.strip().startswith("+")
    ][0]

    assert (
        first_suggestion_line == expected_suggestion
    ), f"Expected first suggestion to be '{expected_suggestion}', got '{first_suggestion_line}'"


class Tests(unittest.TestCase):
    def test_single_source_file(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]"

        args = ["wrapper_hook", "test", "+source1"]

        result = test_runner_interface(args, False, buildozer_output)

        assert result == [
            "test",
            "//some:test",
            "--test_arg=--fileNameFilter",
            "--test_arg=source1",
        ]

    def test_double_source_file(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]"

        args = ["wrapper_hook", "test", "+source1", "+source2"]

        result = test_runner_interface(args, False, buildozer_output)

        assert result == [
            "test",
            "//some:test",
            "--test_arg=--fileNameFilter",
            "--test_arg=source1|source2",
        ]

    def test_duplicate_source_file(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]"

        args = ["wrapper_hook", "test", "+source1", "+source1"]

        result = test_runner_interface(args, False, buildozer_output)

        assert result == [
            "test",
            "//some:test",
            "--test_arg=--fileNameFilter",
            "--test_arg=source1",
        ]

    def test_no_plus_targets(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]"

        args = ["wrapper_hook", "test", "source1", "source1"]

        result = test_runner_interface(args, False, buildozer_output)

        assert result == ["test", "source1", "source1"]

    def test_plus_option(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]"

        args = [
            "wrapper_hook",
            "test",
            "+source1",
            "+source2",
            "//some:other_target",
            "--features",
            "+some_feature",
        ]

        stderr_capture = StringIO()
        with redirect_stderr(stderr_capture):
            result = test_runner_interface(args, False, buildozer_output)

        assert result == [
            "test",
            "//some:test",
            "//some:other_target",
            "--features",
            "+some_feature",
            "--test_arg=--fileNameFilter",
            "--test_arg=source1|source2",
        ]

        # Verify that the warning for +some_feature was captured
        stderr_output = stderr_capture.getvalue()
        assert "WARNING: Target '+some_feature' not found" in stderr_output

    def test_single_bin_file(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]"

        args = ["wrapper_hook", "test", "+test"]

        result = test_runner_interface(args, False, buildozer_output)

        assert result == ["test", "//some:test"]

    def test_double_bin_file(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]\n//some:test2 [source3.cpp source4.cpp]"

        args = ["wrapper_hook", "test", "+test", "+test2"]

        result = test_runner_interface(args, False, buildozer_output)

        assert result == ["test", "//some:test", "//some:test2"]

    def test_bin_source_redundant_mix(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]"

        args = ["wrapper_hook", "test", "+test", "+source2"]

        result = test_runner_interface(args, False, buildozer_output)

        assert result == ["test", "//some:test"]

    def test_bin_source_mix(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]\n//some:test2 [source3.cpp source4.cpp]"

        args = ["wrapper_hook", "test", "+test", "+source3"]

        with self.assertRaises(BinAndSourceIncompatible):
            test_runner_interface(args, False, buildozer_output)

    def test_duplicate_source_names(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]\n//some:test2 [source1.cpp source4.cpp]"

        args = ["wrapper_hook", "test", "+test", "+source3"]

        with self.assertRaises(DuplicateSourceNames):
            test_runner_interface(args, False, buildozer_output)

    def test_autocomplete(self):
        if "linux" not in sys.platform:
            self.skipTest("Skipping because not linux")

        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp]"

        args = ["wrapper_hook", "query", "some_autocomplete_query", "+wrench", "+source1"]

        result = test_runner_interface(args, True, buildozer_output)

        assert result == ["query", "some_autocomplete_query", "+wrench", "+source1"]

    def test_select_statement(self):
        def buildozer_output(autocomplete_query):
            return """//some/select:test [
    "source1.cpp",
] + select({
    "//some:config": [
        "source2.cpp",
    ],
    "//some:other_config": [
        "source3.cpp",
    ],
}) + [
    "source4.cpp",
    "source5.cpp",
]"""

        args = ["wrapper_hook", "test", "+source1", "+source2", "+source3", "+source4"]

        result = test_runner_interface(args, False, buildozer_output)
        assert result == [
            "test",
            "//some/select:test",
            "--test_arg=--fileNameFilter",
            "--test_arg=source1|source2|source3|source4",
        ]

    def test_c_extensions(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.c source2.h source3.cpp source4.cc]"

        args = ["wrapper_hook", "test", "+source1", "+source2", "+source3", "+source4"]

        stderr_capture = StringIO()
        with redirect_stderr(stderr_capture):
            result = test_runner_interface(args, False, buildozer_output)

        assert result == [
            "test",
            "//some:test",
            "+source2",
            "--test_arg=--fileNameFilter",
            "--test_arg=source1|source3|source4",
        ]

        # Verify that the warning for +source2 was captured
        stderr_output = stderr_capture.getvalue()
        assert "WARNING: Target '+source2' not found" in stderr_output

    def test_prefixes(self):
        def buildozer_output(autocomplete_query):
            return "//some:test [source1.cpp source2.cpp source3.cpp s+ource4.cpp]"

        args = ["wrapper_hook", "test", "//:+source1", ":+source2", "+source3"]

        result = test_runner_interface(args, False, buildozer_output)

        assert result == [
            "test",
            "//some:test",
            "--test_arg=--fileNameFilter",
            "--test_arg=source1|source2|source3",
        ]

    def test_target_not_found_with_suggestions(self):
        """Test that unrecognized targets pass through unchanged (not a test target)."""

        def buildozer_output(autocomplete_query):
            return "//some:test [bson_obj_test.cpp bson_element_test.cpp other_test.cpp]"

        args = ["wrapper_hook", "test", "+bsonobj_test"]  # Typo: missing underscore

        stderr_capture = StringIO()
        with redirect_stderr(stderr_capture):
            result = test_runner_interface(args, False, buildozer_output)

        # Should pass through unchanged since it's not a recognized target
        assert result == ["test", "+bsonobj_test"]

        # Check that suggestions were printed
        stderr_output = stderr_capture.getvalue()
        validate_first_suggestion(stderr_output, "+bson_obj_test")

    def test_target_not_found_no_close_matches(self):
        """Test that completely unrecognized targets pass through unchanged."""

        def buildozer_output(autocomplete_query):
            return "//some:test [bson_obj_test.cpp other_test.cpp]"

        args = ["wrapper_hook", "test", "+xyz123"]  # Completely different target

        stderr_capture = StringIO()
        with redirect_stderr(stderr_capture):
            result = test_runner_interface(args, False, buildozer_output)

        # Should pass through unchanged
        assert result == ["test", "+xyz123"]

        # Check that "no similar targets" message was printed
        stderr_output = stderr_capture.getvalue()
        assert (
            "and no similar targets" in stderr_output
        ), f"Expected 'and no similar targets' in stderr output, got: {stderr_output}"

    def test_target_not_found_partial_match(self):
        """Test that partial matches still pass through when not found."""

        def buildozer_output(autocomplete_query):
            return "//some:test [bson_obj_test.cpp bson_element_test.cpp bson_utf8_test.cpp]"

        args = ["wrapper_hook", "test", "+bson_obj"]  # Missing '_test' suffix

        stderr_capture = StringIO()
        with redirect_stderr(stderr_capture):
            result = test_runner_interface(args, False, buildozer_output)

        # Should pass through unchanged
        assert result == ["test", "+bson_obj"]

        stderr_output = stderr_capture.getvalue()
        validate_first_suggestion(stderr_output, "+bson_obj_test")


if __name__ == "__main__":
    unittest.main()
