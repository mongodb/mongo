import errno
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.append(os.path.dirname(__file__))

import validate_compile_commands as validator


class ValidateCompileCommandsTest(unittest.TestCase):
    def test_validation_output_dir_uses_workspace_when_writable(self):
        with tempfile.TemporaryDirectory() as workspace_dir:
            with mock.patch.dict(os.environ, {}, clear=True):
                output_dir = validator._validation_output_dir(workspace_dir)

        self.assertEqual(output_dir, os.path.join(workspace_dir, ".validate_compile_commands_out"))

    def test_validation_output_dir_falls_back_when_workspace_is_read_only(self):
        with tempfile.TemporaryDirectory() as workspace_dir:
            fallback_dir = "/tmp/validate_compile_commands_out"
            with mock.patch.dict(os.environ, {}, clear=True):
                with mock.patch.object(
                    validator.os,
                    "makedirs",
                    side_effect=OSError(errno.EROFS, "read-only file system"),
                ):
                    with mock.patch.object(
                        validator.tempfile, "mkdtemp", return_value=fallback_dir
                    ):
                        output_dir = validator._validation_output_dir(workspace_dir)

        self.assertEqual(output_dir, fallback_dir)

    def test_validation_output_dir_uses_container_shared_install_mount(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            shared_install_dir = os.path.join(temp_dir, "shared-install")
            with mock.patch.dict(
                os.environ,
                {validator.CONTAINER_SHARED_INSTALL_ENV: shared_install_dir},
                clear=True,
            ):
                output_dir = validator._validation_output_dir(temp_dir)

        self.assertEqual(
            output_dir,
            os.path.join(shared_install_dir, "validate_compile_commands_out"),
        )

    def test_validation_output_dir_infers_container_shared_install_mount(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_base = os.path.join(temp_dir, "bazel-output")
            shared_install_dir = output_base + validator.CONTAINER_SHARED_INSTALL_SUFFIX
            os.makedirs(shared_install_dir)
            compdb_path = os.path.join(temp_dir, "compile_commands.json")
            with open(compdb_path, "w", encoding="utf-8") as compdb:
                json.dump(
                    [
                        {
                            "directory": temp_dir,
                            "file": "src/mongo/example.cpp",
                            "arguments": ["clang++", "-c", "example.cpp"],
                            "output": os.path.join(
                                output_base,
                                "execroot",
                                "_main",
                                "bazel-out",
                                "aarch64-fastbuild",
                                "example.o",
                            ),
                        }
                    ],
                    compdb,
                )

            with mock.patch.dict(os.environ, {}, clear=True):
                output_dir = validator._validation_output_dir(temp_dir, compdb_path)

        self.assertEqual(
            output_dir,
            os.path.join(shared_install_dir, "validate_compile_commands_out"),
        )

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

    def test_spawn_replay_runs_recorded_command_without_container_adaptation(self):
        args = [
            "C:/python/python.exe",
            "C:/repo/bazel/wrapper_hook/compiledb_windows.py",
            "C:/output/config.json",
            "C:/output/external/toolchain/clang-cl.cmd",
            "/IC:/SDK/include",
            "@C:/output/compile.rsp",
        ]
        environment = {"PATH": "host-path"}
        completed = subprocess.CompletedProcess(args, 0, stdout="", stderr="")
        with mock.patch.object(validator.subprocess, "run", return_value=completed) as run:
            validator._spawn_replay_compile(args, "C:/repo", environment)
        run.assert_called_once_with(
            args, cwd="C:/repo", env=environment, capture_output=True, text=True
        )

    def test_compiler_diagnostics_drops_show_includes_notes(self):
        output = (
            "Note: including file: src/mongo/a.h\n"
            "src/mongo/a.cpp(3,5): error: unknown type name 'foo'\n"
            "Note: including file:  src/mongo/b.h\n"
            "1 error generated.\n"
        )
        self.assertEqual(
            validator._compiler_diagnostics(output),
            "src/mongo/a.cpp(3,5): error: unknown type name 'foo'\n1 error generated.",
        )
        self.assertEqual(validator._compiler_diagnostics("x" * 20, limit=5), "xxxxx")

    def test_wrapped_validation_delegates_output_selection_to_the_wrapper(self):
        with tempfile.TemporaryDirectory() as workspace:
            args = [
                "python.exe",
                os.path.join(workspace, "compiledb_windows.py"),
                "config.json",
                "clang-cl.cmd",
                "/c",
                "foo.cpp",
            ]
            entry = {"arguments": args, "directory": workspace, "file": "src/mongo/foo.cpp"}
            completed = subprocess.CompletedProcess(args, 0, stdout="", stderr="")
            with (
                mock.patch.dict(
                    os.environ,
                    {"BUILD_WORKSPACE_DIRECTORY": workspace, "VALIDATE_COMPILE_COMMANDS_JOBS": "1"},
                    clear=True,
                ),
                mock.patch.object(validator.os, "chdir"),
                mock.patch.object(validator, "_ensure_compiledb_exists"),
                mock.patch.object(validator, "_validate_clang_tidy_setup"),
                mock.patch.object(
                    validator, "_parse_args", return_value=mock.Mock(run_all=False, sample_size=1)
                ),
                mock.patch.object(
                    validator, "_select_entries_for_test_compile", return_value=(1000, [entry])
                ),
                mock.patch.object(validator, "_validation_output_dir") as output_dir,
                mock.patch.object(validator, "_maybe_add_windows_toolchain_env") as native_env,
                mock.patch.object(validator, "_ensure_parent_dirs_exist_for_outputs"),
                mock.patch.object(
                    validator, "_spawn_replay_compile", return_value=completed
                ) as spawn,
            ):
                self.assertEqual(validator.main(), 0)
            output_dir.assert_not_called()
            native_env.assert_not_called()
            self.assertEqual(spawn.call_args.args[0], [*args[:2], "--validate", *args[2:]])
            self.assertEqual(spawn.call_args.args[1], workspace)


if __name__ == "__main__":
    unittest.main()
