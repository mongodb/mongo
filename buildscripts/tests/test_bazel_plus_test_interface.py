import sys
import unittest

sys.path.append(".")

from bazel.wrapper_hook.plus_interface import (
    BinAndSourceIncompatible,
    DuplicateSourceNames,
    test_runner_interface,
)


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

        result = test_runner_interface(args, False, buildozer_output)

        assert result == [
            "test",
            "//some:test",
            "+source2",
            "--test_arg=--fileNameFilter",
            "--test_arg=source1|source3|source4",
        ]

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


if __name__ == "__main__":
    unittest.main()
