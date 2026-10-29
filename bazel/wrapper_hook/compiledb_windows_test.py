import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from bazel.wrapper_hook import compiledb_windows as wrapper


class WindowsCompileCommandTest(unittest.TestCase):
    def test_response_file_contents_preserves_and_quotes_arguments(self):
        args = [
            "-I/z/b/b/execroot/_main/bazel-out/x64/bin",
            "/I C:/Program Files (x86)/Windows Kits/10/Include",
            "-DNOSPACE",
            r'-DX="quoted"',
            "",
        ]
        contents = wrapper._response_file_contents(args)

        self.assertEqual(len(contents.splitlines()), 1)
        self.assertEqual(shlex.split(contents), args)

    def test_response_file_contents_preserves_backslashes_for_clang_cl(self):
        args = [r'-DTEXT="a\nb"', r'-DQUOTE="a\"b"', r"-DCHAR='x'", "trailing\\", ""]
        self.assertEqual(
            wrapper._response_file_contents(args),
            r'''-DTEXT=\"a\nb\" -DQUOTE=\"a\\\"b\" -DCHAR='x' trailing\ ""''' + "\n",
        )

    def test_response_file_keeps_clang_option_values_on_the_same_line(self):
        args = ["-Xclang", "-ivfsoverlay", "-Xclang", "/path with spaces/vfs.yaml", "/c", "a.cpp"]
        contents = wrapper._response_file_contents(args)
        # clang-cl treats line breaks as option boundaries: a value-taking option at
        # the end of a line is rejected even if its value starts the next line.
        self.assertEqual(len(contents.splitlines()), 1)
        self.assertEqual(shlex.split(contents), args)

    def test_container_replay_arg_translates_windows_paths(self):
        cases = {
            "/IZ:/b/b/execroot/_main/bazel-out/x64/bin": "/I/z/b/b/execroot/_main/bazel-out/x64/bin",
            r"/IZ:\b\b\external\windows_sasl": "/I/z/b/b/external/windows_sasl",
            "/external:IC:/Program Files/Kits": "/external:I/c/Program Files/Kits",
            r"/FoZ:\data\mci\src\.validate_compile_commands\foo.obj": (
                "/Fo/z/data/mci/src/.validate_compile_commands/foo.obj"
            ),
            "Z:/b/b/external/toolchain/case_insensitive_vfs.yaml": (
                "/z/b/b/external/toolchain/case_insensitive_vfs.yaml"
            ),
            "-isystemC:/sdk/include": "-isystem/c/sdk/include",
            '-DMONGO_ROOT="Z:/data/mci/src"': '-DMONGO_ROOT="/z/data/mci/src"',
            "-DBUILD_URL=https://example.com/x": "-DBUILD_URL=https://example.com/x",
            "/I.": "/I.",
            "/Ibazel-out/x64/bin": "/Ibazel-out/x64/bin",
            "src/mongo/db/commands/test_commands.cpp": "src/mongo/db/commands/test_commands.cpp",
            "-Xclang": "-Xclang",
            "/std:c++20": "/std:c++20",
        }
        for arg, expected in cases.items():
            with self.subTest(arg=arg):
                self.assertEqual(wrapper._container_replay_arg(arg), expected)

    def test_container_replay_arg_preserves_non_path_backslashes(self):
        cases = {
            r'-DTEXT="a\nb"': r'-DTEXT="a\nb"',
            r'-DQUOTE="a\"b"': r'-DQUOTE="a\"b"',
            r'-DMONGO_ROOT="Z:\data\mci\src"': '-DMONGO_ROOT="/z/data/mci/src"',
            r'-DMONGO_ROOT="C:\Program Files\MongoDB"': '-DMONGO_ROOT="/c/Program Files/MongoDB"',
            r"/Isrc\mongo": "/Isrc/mongo",
        }
        for arg, expected in cases.items():
            with self.subTest(arg=arg):
                self.assertEqual(wrapper._container_replay_arg(arg), expected)

    def test_container_replay_arg_honours_the_drive_mount_prefix(self):
        cases = {
            "/IZ:/b/b/execroot/_main/bazel-out/x64/bin": "/I/mnt/z/b/b/execroot/_main/bazel-out/x64/bin",
            r"Z:\b\b-mongo-shared-install\out\foo.o": "/mnt/z/b/b-mongo-shared-install/out/foo.o",
            '-DMONGO_ROOT="C:/data/mci/src"': '-DMONGO_ROOT="/mnt/c/data/mci/src"',
            "/Isrc/third_party/boost": "/Isrc/third_party/boost",
        }
        for arg, expected in cases.items():
            with self.subTest(arg=arg):
                self.assertEqual(wrapper._container_replay_arg(arg, "/mnt"), expected)

    def test_windows_response_file_round_trip(self):
        args = [
            r'-DTEXT="a\nb"',
            r'-DQUOTE="a\"b"',
            "",
            "trailing\\",
            "two words",
            "C:\\dir with spaces\\",
            "-DCHAR='x'",
        ]
        self.assertEqual(wrapper._split_response_file(wrapper._response_file_contents(args)), args)

    def _fixture(self, root: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, dict]:
        workspace = root / "src"
        script = workspace / "bazel" / "wrapper_hook" / "compiledb_windows.py"
        script.parent.mkdir(parents=True)
        shutil.copyfile(wrapper.__file__, script)
        runner = workspace / wrapper.WINDOWS_CONTAINER_ACTION_RUNNER
        runner.parent.mkdir(parents=True)
        runner.write_text(
            "import json, os, pathlib, subprocess, sys\n"
            "config = json.loads(pathlib.Path(sys.argv[1]).read_text())\n"
            "pathlib.Path(config['record']).write_text(json.dumps({\n"
            "    'args': sys.argv[2:], 'cwd': os.getcwd(), 'environment': dict(os.environ)}))\n"
            "if config.get('real_compiler'):\n"
            "    sys.exit(subprocess.run([config['real_compiler'], '--driver-mode=cl', *sys.argv[3:]]).returncode)\n"
            "sys.exit(config.get('exit_code', 0))\n",
            encoding="utf-8",
        )
        output_base = root / "output"
        toolchain = output_base / "external" / "mongo_windows_cross_toolchain_files"
        (toolchain / "wrappers").mkdir(parents=True)
        (toolchain / "BUILD.bazel").write_text(
            'cxx_builtin_include_directories = ["external/mongo_windows_cross_toolchain_files/include"]\n'
            'msvc_env_lib = "external/mongo_windows_cross_toolchain_files/lib"\n',
            encoding="utf-8",
        )
        config = {
            "host_system": "Windows",
            "runner_python": sys.executable,
            "shared_install_dir": str(root / "shared install"),
            "record": str(root / "record.json"),
        }
        config_path = output_base / wrapper.WINDOWS_CONTAINER_ACTIONS_CONFIG_FILENAME
        config_path.write_text(json.dumps(config), encoding="utf-8")
        compiler = toolchain / "wrappers" / "clang-cl.cmd"
        return workspace, compiler, config

    def test_generated_command_runs_from_shell_with_its_own_environment_and_writable_outputs(self):
        for long_command in (False, True):
            with self.subTest(long_command=long_command), tempfile.TemporaryDirectory() as temp:
                root = pathlib.Path(temp)
                workspace, compiler, config = self._fixture(root)
                (workspace / "foo.cpp").write_text(
                    'static_assert(sizeof(TEXT) == 4, "macro changed");\n', encoding="utf-8"
                )
                # The test runner simulates container transport; a Windows host Clang
                # cannot consume /mnt paths. Linux also exercises the real compiler.
                config["real_compiler"] = shutil.which("clang") if os.name != "nt" else None
                config_path = (
                    compiler.parents[3] / wrapper.WINDOWS_CONTAINER_ACTIONS_CONFIG_FILENAME
                )
                config_path.write_text(json.dumps(config), encoding="utf-8")
                original_output = root / "output" / "execroot" / "_main" / "foo.obj"
                args = [
                    str(compiler),
                    r'-DTEXT="a\nb"',
                    "/c",
                    "foo.cpp",
                    "/Fo" + str(original_output),
                ]
                if long_command:
                    args.extend(["/IC:/Program Files/SDK/include"] * 350)
                entries = [
                    {
                        "arguments": args,
                        "directory": str(workspace),
                        "file": "foo.cpp",
                        "output": str(original_output),
                    }
                ]
                with mock.patch.object(wrapper.platform, "system", return_value="Windows"):
                    wrapper.wrap_windows_compile_commands(entries, str(workspace))
                command = entries[0]["arguments"]
                self.assertLess(
                    len(subprocess.list2cmdline(command)), wrapper.WINDOWS_CMD_LENGTH_LIMIT
                )
                if long_command:
                    self.assertTrue(command[-1].startswith("@"))
                shell_command = (
                    subprocess.list2cmdline(command) if os.name == "nt" else shlex.join(command)
                )
                environment = dict(
                    os.environ,
                    INCLUDE="wrong-native-sdk",
                    LIB="wrong-native-lib",
                    UNRELATED_HOST_VAR="must-not-leak",
                )
                result = subprocess.run(
                    shell_command,
                    shell=True,
                    cwd=workspace,
                    env=environment,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                record = json.loads(pathlib.Path(config["record"]).read_text())
                self.assertEqual(record["cwd"], str(workspace))
                self.assertEqual(record["args"][0], str(compiler))
                self.assertIn(r'-DTEXT="a\nb"', record["args"])
                self.assertIn(
                    "/Fo" + wrapper._container_replay_arg(entries[0]["output"], "/mnt"),
                    record["args"],
                )
                self.assertTrue(pathlib.Path(entries[0]["output"]).parent.is_dir())
                if config["real_compiler"]:
                    self.assertTrue(pathlib.Path(entries[0]["output"]).is_file())
                self.assertTrue(entries[0]["output"].startswith(config["shared_install_dir"]))
                self.assertNotIn("UNRELATED_HOST_VAR", record["environment"])
                self.assertNotIn("wrong-native-sdk", record["environment"]["INCLUDE"])
                self.assertNotIn("wrong-native-lib", record["environment"]["LIB"])
                if long_command:
                    self.assertIn("/I/mnt/c/Program Files/SDK/include", record["args"])
                # The same wrapper also exposes output redirection to validation.
                validate_output = root / "temporary outputs"
                validation = wrapper.validation_command(command, str(validate_output))
                result = subprocess.run(
                    validation, cwd=workspace, env=environment, capture_output=True, text=True
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                record = json.loads(pathlib.Path(config["record"]).read_text())
                self.assertTrue(
                    any(
                        a.startswith(
                            "/Fo" + wrapper._container_replay_arg(str(validate_output), "/mnt")
                        )
                        for a in record["args"]
                    )
                )

    def test_wrapper_preserves_compiler_failure_status(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            workspace, compiler, config = self._fixture(root)
            config["exit_code"] = 42
            config_path = compiler.parents[3] / wrapper.WINDOWS_CONTAINER_ACTIONS_CONFIG_FILENAME
            config_path.write_text(json.dumps(config), encoding="utf-8")
            script = workspace / "bazel" / "wrapper_hook" / "compiledb_windows.py"
            result = subprocess.run(
                [sys.executable, str(script), str(config_path), str(compiler), "/c", "foo.cpp"],
                cwd=workspace,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 42, result.stderr)

    def test_validation_defaults_to_the_configured_writable_mount(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            workspace, compiler, config = self._fixture(root)
            original = root / "output" / "execroot" / "_main" / "foo.obj"
            entry = {
                "arguments": [str(compiler), "/c", "foo.cpp", "/Fo" + str(original)],
                "directory": str(workspace),
                "file": "foo.cpp",
                "output": str(original),
            }
            with mock.patch.object(wrapper.platform, "system", return_value="Windows"):
                wrapper.wrap_windows_compile_commands([entry], str(workspace))
            result = subprocess.run(
                wrapper.validation_command(entry["arguments"]),
                cwd=workspace,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            record = json.loads(pathlib.Path(config["record"]).read_text())
            expected = wrapper._container_replay_arg(
                os.path.join(config["shared_install_dir"], "validate_compile_commands_out"), "/mnt"
            )
            self.assertTrue(any(arg.startswith("/Fo" + expected) for arg in record["args"]))
            self.assertFalse((workspace / ".validate_compile_commands_out").exists())

    def test_generated_command_replays_case_insensitive_headers_from_workspace(self):
        clang = shutil.which("clang")
        if not clang or os.name == "nt":
            self.skipTest("Real Linux Clang is needed to reproduce case-sensitive header lookup")
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            workspace, compiler, config = self._fixture(root)
            config["real_compiler"] = clang
            config_path = compiler.parents[3] / wrapper.WINDOWS_CONTAINER_ACTIONS_CONFIG_FILENAME
            config_path.write_text(json.dumps(config), encoding="utf-8")
            toolchain = compiler.parents[1]
            headers = toolchain / "include"
            headers.mkdir()
            (headers / "Actual.h").write_text("constexpr int value = 1;\n", encoding="utf-8")
            relative = "external/" + toolchain.name + "/include"
            overlay = {
                "version": 0,
                "roots": [
                    {
                        "type": "directory",
                        "name": relative,
                        "contents": [
                            {
                                "type": "file",
                                "name": "actual.h",
                                "external-contents": relative + "/Actual.h",
                            }
                        ],
                    }
                ],
            }
            overlay_path = toolchain / "case_insensitive_vfs.yaml"
            original_overlay = json.dumps(overlay)
            overlay_path.write_text(original_overlay, encoding="utf-8")
            (workspace / "foo.cpp").write_text(
                '#include <actual.h>\nstatic_assert(value == 1, "header");\n', encoding="utf-8"
            )
            args = [
                str(compiler),
                "-Xclang",
                "-ivfsoverlay",
                "-Xclang",
                str(overlay_path),
                "/I" + str(headers),
                "/c",
                "foo.cpp",
                "/Fo" + str(root / "foo.obj"),
            ]
            broken = subprocess.run(
                [clang, "--driver-mode=cl", *args[1:]],
                cwd=workspace,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(broken.returncode, 0)
            self.assertIn("'actual.h' file not found", broken.stderr)
            for long_command in (False, True):
                with self.subTest(long_command=long_command):
                    entry = {
                        "arguments": args + (["/I" + str(headers)] * 400 if long_command else []),
                        "directory": str(workspace),
                        "file": "foo.cpp",
                        "output": str(root / "foo.obj"),
                    }
                    with mock.patch.object(wrapper.platform, "system", return_value="Windows"):
                        wrapper.wrap_windows_compile_commands([entry], str(workspace))
                    if long_command:
                        self.assertTrue(entry["arguments"][-1].startswith("@"))
                    for command in (
                        entry["arguments"],
                        wrapper.validation_command(entry["arguments"]),
                    ):
                        result = subprocess.run(
                            command, cwd=workspace, capture_output=True, text=True
                        )
                        self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(overlay_path.read_text(), original_overlay)

    def test_replay_overlay_uses_absolute_container_paths_and_preserves_alias_names(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            workspace, compiler, config = self._fixture(root)
            toolchain = compiler.parents[1]
            overlay_path = toolchain / "case_insensitive_vfs.yaml"
            overlay = {
                "version": 0,
                "roots": [
                    {
                        "type": "directory",
                        "name": "external/toolchain/include",
                        "contents": [
                            {
                                "type": "file",
                                "name": "Actual.h",
                                "external-contents": "Z:/b/external/toolchain/include/actual.h",
                            }
                        ],
                    }
                ],
            }
            overlay_path.write_text(json.dumps(overlay), encoding="utf-8")
            replay = wrapper.WindowsContainerCompile(
                str(workspace), "Z:/b", "Z:/b/config.json", config, str(toolchain)
            )
            args = [str(compiler), "-Xclang", "-ivfsoverlay", "-Xclang", str(overlay_path)]
            first = replay.replay_overlay_args(args)
            self.assertEqual(first, replay.replay_overlay_args(args))
            rewritten = json.loads(pathlib.Path(first[-1]).read_text())
            node = rewritten["roots"][0]
            self.assertEqual(node["name"], "/mnt/z/b/external/toolchain/include")
            self.assertEqual(node["contents"][0]["name"], "Actual.h")
            self.assertEqual(
                node["contents"][0]["external-contents"],
                "/mnt/z/b/external/toolchain/include/actual.h",
            )
            self.assertTrue(first[-1].startswith(config["shared_install_dir"]))

    def test_wrapper_uses_a_temporary_container_response_file_for_long_child_command(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            workspace, compiler, config = self._fixture(root)
            config_path = compiler.parents[3] / wrapper.WINDOWS_CONTAINER_ACTIONS_CONFIG_FILENAME
            args = [
                str(config_path),
                str(compiler),
                "/IC:/Program Files/SDK/include",
                r'-DTEXT="a\nb"',
                "/c",
                "foo.cpp",
            ]
            captured = {}

            def run(command, **kwargs):
                self.assertEqual(len(command), 5)
                self.assertTrue(command[-1].startswith("@"))
                response = next(
                    (
                        pathlib.Path(config["shared_install_dir"]) / wrapper.RESPONSE_FILE_DIRNAME
                    ).glob("*.rsp")
                )
                self.assertEqual(
                    command[-1], "@" + wrapper._container_replay_arg(str(response), "/mnt")
                )
                captured["path"] = response
                captured["args"] = wrapper._split_response_file(response.read_text())
                return subprocess.CompletedProcess(command, 0)

            with (
                mock.patch.object(wrapper, "WINDOWS_CONTAINER_COMMAND_LENGTH_LIMIT", 1),
                mock.patch.object(wrapper.subprocess, "run", side_effect=run),
            ):
                self.assertEqual(wrapper.main(args), 0)
            self.assertIn("/I/mnt/c/Program Files/SDK/include", captured["args"])
            self.assertIn(r'-DTEXT="a\nb"', captured["args"])
            self.assertFalse(captured["path"].exists())

    def test_generation_fails_if_cross_container_setup_is_missing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            workspace, compiler, _ = self._fixture(root)
            (workspace / wrapper.WINDOWS_CONTAINER_ACTION_RUNNER).unlink()
            entries = [
                {
                    "arguments": [str(compiler), "/c", "foo.cpp"],
                    "directory": str(workspace),
                    "file": "foo.cpp",
                }
            ]
            with mock.patch.object(wrapper.platform, "system", return_value="Windows"):
                with self.assertRaisesRegex(RuntimeError, "runner does not exist"):
                    wrapper.wrap_windows_compile_commands(entries, str(workspace))

    def test_native_entries_are_preserved(self):
        entries = [
            {"arguments": ["cl.exe", "/c", "foo.cpp"], "directory": "C:/src", "file": "foo.cpp"}
        ]
        with mock.patch.object(wrapper.platform, "system", return_value="Windows"):
            self.assertEqual(wrapper.wrap_windows_compile_commands(entries, "C:/src"), entries)

    def test_all_output_flags_are_redirected_without_changing_pch_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            args = [
                "clang-cl.cmd",
                "-o",
                "foo.obj",
                "-MF",
                "different-name.d",
                "/Fdfoo.pdb",
                "/Fafoo.asm",
                "/Fifoo.i",
                "/Fpinput.pch",
                "/Iheaders",
                "foo.cpp",
            ]
            result = wrapper._rewrite_output_paths_to_writable_dir(
                args, temp, os.path.join(temp, "out")
            )
            for index in (2, 4):
                self.assertTrue(result[index].startswith(os.path.join(temp, "out")))
            for index in (5, 6, 7):
                self.assertTrue(result[index][3:].startswith(os.path.join(temp, "out")))
            self.assertEqual(result[8:], args[8:])

    def test_response_file_arguments_compile_with_real_clang_cl(self):
        clang = shutil.which("clang")
        if not clang:
            self.skipTest("Clang is not installed")
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "foo.cpp").write_text(
                'static_assert(sizeof(TEXT) == 4, "macro changed");\n', encoding="utf-8"
            )
            args = [r'-DTEXT="a\nb"', "/c", "foo.cpp", "/Fofoo.obj"]
            (root / "compile.rsp").write_text(
                wrapper._response_file_contents(args), encoding="utf-8"
            )
            result = subprocess.run(
                [clang, "--driver-mode=cl", "@compile.rsp"],
                cwd=root,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((root / "foo.obj").is_file())


if __name__ == "__main__":
    unittest.main()
