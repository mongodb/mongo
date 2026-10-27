"""Check execution requirements against the Rust compiler's execution OS."""

import ast
import re
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Any

WRAPPER = Path(sys.argv.pop(1))
BUILD_SCRIPT = Path(sys.argv.pop(1))


class WindowsCrossExecutionTest(unittest.TestCase):
    def test_copy_action_routes_by_copy_tool_os(self) -> None:
        tree = ast.parse(BUILD_SCRIPT.read_text())
        function = next(
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef)
            and node.name == "_cargo_build_script_runfiles_impl"
        )
        namespace = {}
        helper = Path(__file__).with_name("windows_cross_execution.bzl")
        exec(compile(helper.read_text(), str(helper), "exec"), namespace)
        setting_info = object()
        namespace["BuildSettingInfo"] = setting_info
        exec(
            compile(ast.Module(body=[function], type_ignores=[]), str(BUILD_SCRIPT), "exec"),
            namespace,
        )

        class ActionRecorded(Exception):
            pass

        for enabled in (False, True):
            for copy_extension in ("exe", ""):
                with self.subTest(enabled=enabled, copy_extension=copy_extension):
                    recorded = {}

                    def record(**kwargs: Any) -> None:
                        recorded.update(kwargs)
                        raise ActionRecorded

                    ctx = SimpleNamespace(
                        executable=SimpleNamespace(
                            script=SimpleNamespace(extension="exe"),
                            _copy_file=SimpleNamespace(extension=copy_extension),
                        ),
                        label=SimpleNamespace(name="fixture"),
                        attr=SimpleNamespace(
                            _mongo_windows_cross_local_container_actions={
                                setting_info: SimpleNamespace(value=enabled),
                            },
                        ),
                        actions=SimpleNamespace(
                            declare_file=lambda name: name,
                            args=lambda: SimpleNamespace(add=lambda value: None),
                            run=record,
                        ),
                    )
                    with self.assertRaises(ActionRecorded):
                        namespace[function.name](ctx)
                    self.assertEqual(recorded["mnemonic"], "CargoBuildScriptCopy")
                    self.assertEqual(
                        recorded["execution_requirements"],
                        {"no-remote-exec": "1"} if enabled and copy_extension == "exe" else {},
                    )

    def test_linux_actions_replace_windows_path_but_preserve_linux_and_native_environments(
        self,
    ) -> None:
        namespace = {}
        source = Path(__file__).with_name("windows_cross_execution.bzl")
        exec(compile(source.read_text(), str(source), "exec"), namespace)
        environment = namespace["windows_cross_action_environment"]
        for path in ("C:/cygwin/bin;C:/Windows", "C:/cygwin/bin", "C:\\cygwin\\bin"):
            original = {"PATH": path, "CARGO_CFG_TARGET_OS": "linux"}
            self.assertEqual(
                environment(True, "linux", original),
                {"PATH": "/usr/local/bin:/usr/bin:/bin", "CARGO_CFG_TARGET_OS": "linux"},
            )
            self.assertEqual(original["PATH"], path)
            self.assertEqual(environment(False, "linux", original), original)
            self.assertEqual(environment(True, "windows", original), original)
        self.assertEqual(environment(True, "linux", {}), {"PATH": "/usr/local/bin:/usr/bin:/bin"})
        original = {"PATH": "/toolchain/bin:/usr/bin:/bin"}
        self.assertEqual(environment(True, "linux", original), original)

    def test_registered_patches_have_valid_hunk_counts(self) -> None:
        for patch in Path(__file__).parent.glob("*.patch"):
            lines = patch.read_text().splitlines()
            for index, line in enumerate(lines):
                if not line.startswith("@@ "):
                    continue
                header = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", line)
                self.assertIsNotNone(header, f"{patch}:{index + 1}")
                old_count = new_count = 0
                for body_line in lines[index + 1 :]:
                    if not body_line or body_line.startswith(("@@ ", "diff --git ")):
                        break
                    old_count += body_line[0] in " -"
                    new_count += body_line[0] in " +"
                with self.subTest(patch=patch.name, line=index + 1):
                    self.assertEqual(old_count, int(header[2] or 1))
                    self.assertEqual(new_count, int(header[4] or 1))

    def test_native_windows_cc_tools_use_absolute_toolchain_paths(self) -> None:
        def fail(message: str) -> None:
            raise ValueError(message)

        namespace = {"fail": fail}
        source = Path(__file__).with_name("windows_cross_execution.bzl")
        exec(compile(source.read_text(), str(source), "exec"), namespace)
        environment = namespace["windows_cross_native_cc_environment"]
        original = {
            "CC": "external/toolchain/C:/Program Files/MSVC/cl.exe",
            "CXX": "external/toolchain/C:/Program Files/MSVC/cl.exe",
            "AR": "external/toolchain/C:/Program Files/MSVC/lib.exe",
            "LD": "external/toolchain/C:/Program Files/MSVC/link.exe",
            "PATH": "C:/untrusted;C:/Program Files/MSVC",
        }
        self.assertEqual(
            environment(True, "windows", original),
            {
                "CC": "C:/Program Files/MSVC/cl.exe",
                "CXX": "C:/Program Files/MSVC/cl.exe",
                "AR": "C:/Program Files/MSVC/lib.exe",
                "LD": "C:/Program Files/MSVC/link.exe",
                "PATH": original["PATH"],
            },
        )
        self.assertEqual(original["CC"], "external/toolchain/C:/Program Files/MSVC/cl.exe")
        self.assertEqual(environment(False, "windows", original), original)
        self.assertEqual(environment(True, "linux", original), original)
        for path in ("D:\\MSVC\\cl.exe", "//server/MSVC/cl.exe", "C:/MSVC/cl.exe"):
            with self.subTest(path=path):
                tools = {key: path for key in ("CC", "CXX", "AR", "LD")}
                self.assertEqual(
                    environment(True, "windows", tools),
                    {key: path.replace("\\", "/") for key in tools},
                )
        for path in ("cl.exe", "external/toolchain/cl.exe"):
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "absolute native"):
                environment(True, "windows", {key: path for key in ("CC", "CXX", "AR", "LD")})

    def test_only_windows_tools_are_kept_off_linux_rbe(self) -> None:
        namespace = {}
        source = Path(__file__).with_name("windows_cross_execution.bzl")
        exec(compile(source.read_text(), str(source), "exec"), namespace)
        requirements = namespace["windows_cross_execution_requirements"]
        for enabled in (False, True):
            for exec_os in ("windows", "linux", "darwin"):
                with self.subTest(enabled=enabled, exec_os=exec_os):
                    self.assertEqual(
                        requirements(enabled, exec_os),
                        {"no-remote-exec": "1"} if enabled and exec_os == "windows" else {},
                    )

    def test_native_windows_cc_accepts_execroot_relative_rust_linker(self) -> None:
        def fail(message: str) -> None:
            raise ValueError(message)

        namespace = {"fail": fail}
        source = Path(__file__).with_name("windows_cross_execution.bzl")
        exec(compile(source.read_text(), str(source), "exec"), namespace)
        # get_linker_and_args uses the Rust toolchain's File.path when the
        # Windows cross wrapper sets toolchain_linker_preference=rust.
        linker = (
            "bazel-out/x64_windows-opt/bin/external/rules_rust++rust+rw-tools/"
            "rust_toolchain/bin/rust-lld.exe"
        )
        original = {
            "CC": "external/msvc/C:/Program Files/MSVC/cl.exe",
            "CXX": "external/msvc/C:/Program Files/MSVC/cl.exe",
            "AR": "external/msvc/C:/Program Files/MSVC/lib.exe",
            "LD": linker,
        }
        environment = namespace["windows_cross_native_cc_environment"]
        self.assertEqual(
            environment(True, "windows", original),
            {
                "CC": "C:/Program Files/MSVC/cl.exe",
                "CXX": "C:/Program Files/MSVC/cl.exe",
                "AR": "C:/Program Files/MSVC/lib.exe",
                "LD": linker,
            },
        )
        self.assertEqual(environment(False, "windows", original), original)
        self.assertEqual(environment(True, "linux", original), original)

    def test_cargo_wrapper_controls_the_script_exec_transition(self) -> None:
        tree = ast.parse(WRAPPER.read_text())
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef))
        for windows_cross in (False, True):
            with self.subTest(windows_cross=windows_cross):
                rules = {}

                def record(**kwargs: Any) -> None:
                    rules[kwargs["name"]] = kwargs

                key = (
                    "@@//bazel/rules_rust:windows_cross_native_cargo"
                    if windows_cross
                    else "//conditions:default"
                )
                namespace = {
                    "rust_binary": record,
                    "cargo_build_script_runfiles": record,
                    "_build_script_run": record,
                    "name_to_crate_name": str,
                    "name_to_pkg_name": str,
                    "depset": lambda values: SimpleNamespace(to_list=lambda: values),
                    "select": lambda choices: choices[key],
                }
                exec(
                    compile(ast.Module(body=[function], type_ignores=[]), str(WRAPPER), "exec"),
                    namespace,
                )
                namespace["cargo_build_script"](name="fixture", data=["target_data"])
                script = rules[rules["fixture"]["script"].removeprefix(":")]
                self.assertEqual(script["script"], ":fixture_")
                self.assertEqual(script["data"], ["target_data"])
                self.assertEqual(
                    script.get("exec_compatible_with", []),
                    ["@platforms//os:windows"] if windows_cross else [],
                )
                self.assertNotIn("exec_compatible_with", rules["fixture_"])
                self.assertNotIn("mongo_windows_cross_cargo_build_script_runner", rules["fixture"])


if __name__ == "__main__":
    unittest.main()
