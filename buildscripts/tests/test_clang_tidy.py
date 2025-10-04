"""Unit tests for clang_tidy.py and apply_clang_tidy_fixes.py."""

import os
import platform
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

sys.path.append("buildscripts")
import apply_clang_tidy_fixes
from clang_tidy import _clang_tidy_executor, _combine_errors
from mongo_toolchain import get_mongo_toolchain


@unittest.skipIf(
    sys.platform == "win32"
    or sys.platform == "darwin"
    or platform.machine().lower() in {"ppc64le", "s390x"},
    reason="clang_tidy.py is only run on linux x86_64 or linux arm64",
)
class TestClangTidy(unittest.TestCase):
    def setUp(self):
        """Create a working directory that contains a header and source that clang-tidy will suggest a replacement for."""

        source_contents = """
#include "clang_tidy_test.h"
#include <iostream>
void f(std::string s) { std::cout << s; }
"""
        header_contents = """
#include <string>
void f(std::string s);
"""

        self.tempdir = tempfile.TemporaryDirectory()
        self.source = os.path.join(self.tempdir.name, "clang_tidy_test.cpp")
        with open(self.source, "w") as source:
            source.write(source_contents)
        self.header = os.path.join(self.tempdir.name, "clang_tidy_test.h")
        with open(self.header, "w") as header:
            header.write(header_contents)

        self.fixes_dir = os.path.join(self.tempdir.name, "fixes")
        os.mkdir(self.fixes_dir)

        toolchain = get_mongo_toolchain(from_bazel=False)
        self.clang_tidy_binary = toolchain.get_tool_path("clang-tidy")
        clang_tidy_cfg = "Checks: 'performance-unnecessary-value-param'"
        self.clang_tidy_cfg = yaml.safe_load(clang_tidy_cfg)

        self.oldpwd = os.getcwd()
        os.chdir(self.tempdir.name)

    def tearDown(self):
        os.chdir(self.oldpwd)
        self.tempdir.cleanup()

    def test_clang_tidy_and_apply_replacements(self):
        _, files_to_parse = _clang_tidy_executor(
            Path(self.source),
            self.clang_tidy_binary,
            self.clang_tidy_cfg,
            self.fixes_dir,
            False,
            "",
        )
        _combine_errors(Path(self.fixes_dir, "clang_tidy_fixes.json"), [files_to_parse])
        apply_clang_tidy_fixes.main([os.path.join(self.fixes_dir, "clang_tidy_fixes.json")])

        with open(self.source, "r") as source:
            self.assertIn(
                "void f(const std::string& s)", source.read(), "The clang tidy fix was not applied."
            )
        with open(self.header, "r") as header:
            self.assertIn(
                "void f(const std::string& s)", header.read(), "The clang tidy fix was not applied."
            )

    def test_source_changed_between_tidy_and_apply_fixes(self):
        _, files_to_parse = _clang_tidy_executor(
            Path(self.source),
            self.clang_tidy_binary,
            self.clang_tidy_cfg,
            self.fixes_dir,
            False,
            "",
        )
        _combine_errors(Path(self.fixes_dir, "clang_tidy_fixes.json"), [files_to_parse])

        # Make a modification to one of the files. The clang tidy fix is no longer safe to apply.
        with open(self.header, "a") as header:
            header.write("\n")

        apply_clang_tidy_fixes.main([os.path.join(self.fixes_dir, "clang_tidy_fixes.json")])

        with open(self.source, "r") as source:
            self.assertIn(
                "void f(std::string s)",
                source.read(),
                "The clang-tidy fix should not have been applied.",
            )
        with open(self.header, "r") as header:
            self.assertIn(
                "void f(std::string s)",
                header.read(),
                "The clang-tidy fix should not have been applied.",
            )


if __name__ == "__main__":
    unittest.main()
