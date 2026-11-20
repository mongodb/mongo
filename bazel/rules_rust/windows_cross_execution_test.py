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
    def test_cargo_prefix_map_uses_the_compiler_driver_syntax(self) -> None:
        tree = ast.parse(BUILD_SCRIPT.read_text())
        prefix_map = next(
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.If)
            and any(
                isinstance(statement, ast.Assign)
                and any(
                    isinstance(target, ast.Name) and target.id == "prefix_map"
                    for target in statement.targets
                )
                for statement in node.body
            )
        )
        code = compile(ast.Module(body=[prefix_map], type_ignores=[]), str(BUILD_SCRIPT), "exec")
        for compiler, expected_map in (
            ("clang-cl", "/clang:-ffile-prefix-map=${pwd}=."),
            ("clang", "-ffile-prefix-map=${pwd}=."),
            ("gcc", "-ffile-prefix-map=${pwd}=."),
            ("msvc-cl", ""),
        ):
            for original in ("", "-DKEEP_EXISTING_FLAG"):
                with self.subTest(compiler=compiler, original=original):
                    env = {"CFLAGS": original, "CXXFLAGS": original}
                    namespace = {
                        "cc_toolchain": SimpleNamespace(compiler=compiler),
                        "env": env,
                    }
                    exec(code, namespace)
                    expected = " ".join(part for part in (original, expected_map) if part)
                    self.assertEqual(env, {"CFLAGS": expected, "CXXFLAGS": expected})

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

    def test_cargo_exposes_execroot_paths_and_uses_portable_manifest_files(self) -> None:
        namespace = {}
        source = Path(__file__).with_name("windows_cross_execution.bzl")
        exec(compile(source.read_text(), str(source), "exec"), namespace)
        env = {"INCLUDE": "external/cross/include;/sdk/include;", "LIB": "external/cross/lib"}
        environment = namespace["windows_cross_cargo_environment"]
        self.assertEqual(
            environment(True, "linux", env),
            {
                "PATH": "/usr/local/bin:/usr/bin:/bin",
                **env,
                "RULES_RUST_CARGO_MANIFEST_COPY": "1",
                "RULES_RUST_SYMLINK_EXEC_ROOT": "1",
            },
        )
        self.assertEqual(environment(False, "linux", env), env)
        self.assertEqual(environment(True, "windows", env), env)

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

    def test_cargo_wrapper_controls_the_script_exec_transition(self) -> None:
        tree = ast.parse(WRAPPER.read_text())
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef))
        for windows_cross in (False, True):
            with self.subTest(windows_cross=windows_cross):
                rules = {}

                def record(**kwargs: Any) -> None:
                    rules[kwargs["name"]] = kwargs

                key = (
                    "@@//bazel/rules_rust:windows_cross_linux_cargo"
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
                    ["@platforms//os:linux"] if windows_cross else [],
                )
                self.assertNotIn("exec_compatible_with", rules["fixture_"])
                self.assertNotIn("mongo_windows_cross_cargo_build_script_runner", rules["fixture"])


if __name__ == "__main__":
    unittest.main()
