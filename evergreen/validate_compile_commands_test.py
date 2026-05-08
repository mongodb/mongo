import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.append(os.path.dirname(__file__))

import validate_compile_commands as validator


class ValidateCompileCommandsTest(unittest.TestCase):
    def test_validate_clang_tidy_setup_skips_unsupported_platforms(self):
        with tempfile.TemporaryDirectory() as workspace_dir:
            with mock.patch.object(
                validator, "_mongo_tidy_checks_supported_platform", return_value=False
            ):
                validator._validate_clang_tidy_setup(workspace_dir)

    def test_validate_clang_tidy_setup_accepts_expected_files(self):
        with tempfile.TemporaryDirectory() as workspace_dir:
            plugin_dir = os.path.join(workspace_dir, "bazel-bin", "src", "mongo", "tools")
            os.makedirs(plugin_dir, exist_ok=True)
            plugin_path = os.path.join(plugin_dir, "libmongo_tidy_checks.so")
            with open(os.path.join(workspace_dir, ".clang-tidy"), "w", encoding="utf-8") as f:
                f.write("Checks: '*'\n")
            with open(plugin_path, "w", encoding="utf-8") as f:
                f.write("plugin")
            with open(
                os.path.join(workspace_dir, ".mongo_checks_module_path"), "w", encoding="utf-8"
            ) as f:
                f.write(plugin_path)

            with mock.patch.object(
                validator, "_mongo_tidy_checks_supported_platform", return_value=True
            ):
                validator._validate_clang_tidy_setup(workspace_dir)

    def test_validate_clang_tidy_setup_rejects_missing_config(self):
        with tempfile.TemporaryDirectory() as workspace_dir:
            with mock.patch.object(
                validator, "_mongo_tidy_checks_supported_platform", return_value=True
            ):
                with self.assertRaisesRegex(ValueError, r"Expected '\.clang-tidy' to exist"):
                    validator._validate_clang_tidy_setup(workspace_dir)

    def test_validate_clang_tidy_setup_rejects_missing_plugin(self):
        with tempfile.TemporaryDirectory() as workspace_dir:
            missing_plugin_path = os.path.join(
                workspace_dir, "bazel-bin", "src", "mongo", "tools", "libmongo_tidy_checks.so"
            )
            with open(os.path.join(workspace_dir, ".clang-tidy"), "w", encoding="utf-8") as f:
                f.write("Checks: '*'\n")
            with open(
                os.path.join(workspace_dir, ".mongo_checks_module_path"), "w", encoding="utf-8"
            ) as f:
                f.write(missing_plugin_path)

            with mock.patch.object(
                validator, "_mongo_tidy_checks_supported_platform", return_value=True
            ):
                with self.assertRaisesRegex(
                    ValueError, r"The mongo_tidy_checks plugin file recorded"
                ):
                    validator._validate_clang_tidy_setup(workspace_dir)

    def test_accepts_standard_arguments_entry(self):
        validator._validate_compiledb_entry(
            {
                "directory": "/repo",
                "file": "src/mongo/db/example.cpp",
                "arguments": ["clang++", "-c", "src/mongo/db/example.cpp"],
                "output": "bazel-out/example.o",
            },
            index=1,
        )

    def test_accepts_standard_command_entry(self):
        validator._validate_compiledb_entry(
            {
                "directory": "/repo",
                "file": "src/mongo/db/example.cpp",
                "command": "clang++ -c src/mongo/db/example.cpp -o bazel-out/example.o",
            },
            index=1,
        )

    def test_rejects_non_standard_keys(self):
        with self.assertRaisesRegex(ValueError, r"non-standard keys \['target'\]"):
            validator._validate_compiledb_entry(
                {
                    "directory": "/repo",
                    "file": "src/mongo/db/example.cpp",
                    "arguments": ["clang++", "-c", "src/mongo/db/example.cpp"],
                    "target": "//src/mongo/db:example",
                },
                index=1,
            )

    def test_rejects_entries_with_both_command_and_arguments(self):
        with self.assertRaisesRegex(ValueError, r"exactly one of 'arguments' or 'command'"):
            validator._validate_compiledb_entry(
                {
                    "directory": "/repo",
                    "file": "src/mongo/db/example.cpp",
                    "arguments": ["clang++", "-c", "src/mongo/db/example.cpp"],
                    "command": "clang++ -c src/mongo/db/example.cpp",
                },
                index=1,
            )

    def test_selection_rejects_non_standard_compile_commands_json(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as compiledb:
            json.dump(
                [
                    {
                        "directory": "/repo",
                        "file": "src/mongo/db/example.cpp",
                        "arguments": ["clang++", "-c", "src/mongo/db/example.cpp"],
                        "target": "//src/mongo/db:example",
                    }
                ],
                compiledb,
            )
            compiledb_path = compiledb.name

        try:
            with self.assertRaisesRegex(ValueError, r"non-standard keys \['target'\]"):
                validator._select_entries_for_test_compile(compiledb_path, n=0)
        finally:
            os.remove(compiledb_path)

    def test_ensure_compiledb_exists_builds_install_wiredtiger_too(self):
        with mock.patch.object(validator.os.path, "exists", return_value=False):
            with mock.patch.object(validator.subprocess, "run") as mock_run:
                validator._ensure_compiledb_exists("compile_commands.json")

        mock_run.assert_called_once_with(
            ["bazel", "build", "compiledb", "install-wiredtiger"], check=True
        )


if __name__ == "__main__":
    unittest.main()
