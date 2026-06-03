"""Unit tests for clang_tidy.py and apply_clang_tidy_fixes.py."""

import json
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
void f(ClangTidyTestClass s) { s.use(); }
"""
        header_contents = """
class ClangTidyTestClass {
public:
    ClangTidyTestClass(const ClangTidyTestClass&);
    ~ClangTidyTestClass();
    void use() const;
};
void f(ClangTidyTestClass s);
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

        toolchain = get_mongo_toolchain(from_bazel=True)
        self.clang_tidy_binary = toolchain.get_tool_path("clang-tidy")
        self.compile_commands = os.path.join(self.tempdir.name, "compile_commands.json")
        with open(self.compile_commands, "w") as compile_commands:
            json.dump(
                [
                    {
                        "directory": self.tempdir.name,
                        "arguments": [
                            toolchain.get_tool_path("clang++"),
                            "-std=c++20",
                            f"-I{self.tempdir.name}",
                            "-c",
                            self.source,
                        ],
                        "file": self.source,
                    }
                ],
                compile_commands,
            )
        clang_tidy_cfg = "Checks: 'performance-unnecessary-value-param'"
        self.clang_tidy_cfg = yaml.safe_load(clang_tidy_cfg)

        self.oldpwd = os.getcwd()
        os.chdir(self.tempdir.name)

    def tearDown(self):
        os.chdir(self.oldpwd)
        self.tempdir.cleanup()

    def _run_clang_tidy_and_combine_errors(self):
        _, files_to_parse = _clang_tidy_executor(
            Path(self.source),
            self.clang_tidy_binary,
            self.clang_tidy_cfg,
            self.fixes_dir,
            False,
            "",
            self.compile_commands,
        )

        if not files_to_parse or not os.path.exists(files_to_parse):
            fail_file = Path(self.source).with_suffix(".fail")
            fail_output = "<no clang-tidy failure output>"
            if fail_file.exists():
                with open(fail_file, "r", errors="replace") as output:
                    fail_output = output.read()
            self.fail(
                f"clang-tidy did not produce fixes file {files_to_parse}. "
                f"Failure output:\n{fail_output}"
            )

        _combine_errors(Path(self.fixes_dir, "clang_tidy_fixes.json"), [files_to_parse])

    def test_clang_tidy_and_apply_replacements(self):
        self._run_clang_tidy_and_combine_errors()
        apply_clang_tidy_fixes.main([os.path.join(self.fixes_dir, "clang_tidy_fixes.json")])

        with open(self.source, "r") as source:
            self.assertIn(
                "void f(const ClangTidyTestClass& s)",
                source.read(),
                "The clang tidy fix was not applied.",
            )
        with open(self.header, "r") as header:
            self.assertIn(
                "void f(const ClangTidyTestClass& s)",
                header.read(),
                "The clang tidy fix was not applied.",
            )

    def test_source_changed_between_tidy_and_apply_fixes(self):
        self._run_clang_tidy_and_combine_errors()

        # Make a modification to one of the files. The clang tidy fix is no longer safe to apply.
        with open(self.header, "a") as header:
            header.write("\n")

        apply_clang_tidy_fixes.main([os.path.join(self.fixes_dir, "clang_tidy_fixes.json")])

        with open(self.source, "r") as source:
            self.assertIn(
                "void f(ClangTidyTestClass s)",
                source.read(),
                "The clang-tidy fix should not have been applied.",
            )
        with open(self.header, "r") as header:
            self.assertIn(
                "void f(ClangTidyTestClass s)",
                header.read(),
                "The clang-tidy fix should not have been applied.",
            )


if __name__ == "__main__":
    unittest.main()
