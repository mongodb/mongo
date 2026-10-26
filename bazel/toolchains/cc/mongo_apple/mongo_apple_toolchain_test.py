"""Regression tests for generated native macOS toolchain BUILD files."""

import ast
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Any
from unittest.mock import Mock


def starlark_fail(message: str) -> None:
    raise ValueError(message)


def load_toolchain_functions() -> dict[str, Any]:
    path = Path(__file__).with_name("mongo_apple_toolchain.bzl")
    functions = [
        node for node in ast.parse(path.read_text()).body if isinstance(node, ast.FunctionDef)
    ]
    namespace = {"json": SimpleNamespace(encode=json.dumps), "Label": str, "fail": starlark_fail}
    helper = path.parent.parent / "libcxx_config.bzl"
    exec(compile(helper.read_text(), str(helper), "exec"), namespace)
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(path), "exec"), namespace)
    return namespace


class MongoAppleToolchainTest(unittest.TestCase):
    def test_custom_llvm_libcxx_header_layouts(self) -> None:
        for layout in ("", "aarch64-apple-darwin21.6.0/"):
            with self.subTest(layout=layout), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                config = root / "include" / layout / "c++/v1/__config_site"
                config.parent.mkdir(parents=True)
                disabled = "  # define\t_LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS  0\n"
                config.write_text(disabled)
                files = {}
                execute = Mock(
                    return_value=SimpleNamespace(
                        return_code=0, stdout="aarch64-apple-darwin21.6.0\n", stderr=""
                    )
                )
                context = SimpleNamespace(
                    name="custom_llvm",
                    os=SimpleNamespace(environ={"LLVM_PATH": str(root)}),
                    path=lambda value: SimpleNamespace(realpath=value, exists=Path(value).exists()),
                    read=lambda value: Path(value).read_text(),
                    file=lambda path, content: files.update({path: content}),
                    execute=execute,
                    report_progress=lambda message: None,
                    template=lambda *args: None,
                )
                success, error = load_toolchain_functions()["_configure_oss_clang_toolchain"](
                    context
                )
                self.assertTrue(success, error)
                self.assertEqual(
                    "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 1\n",
                    files["include/config_override/__config_site"],
                )
                self.assertEqual(disabled, config.read_text())
                self.assertEqual(bool(layout), execute.called)

    def test_unknown_libcxx_availability_definition_fails_clearly(self) -> None:
        with self.assertRaisesRegex(ValueError, "Unsupported libc\\+\\+ vendor"):
            load_toolchain_functions()["enable_libcxx_vendor_availability"](
                "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS UNKNOWN\n"
            )

    def test_missing_libcxx_configuration_has_actionable_error(self) -> None:
        context = SimpleNamespace(
            path=lambda value: SimpleNamespace(exists=False),
            execute=lambda args: SimpleNamespace(return_code=0, stdout="target\n", stderr=""),
        )
        path, error = load_toolchain_functions()["_get_libcxx_config_site"](context, "/custom/llvm")
        self.assertEqual("", path)
        self.assertIn("/custom/llvm/include/c++/v1/__config_site", error)
        self.assertIn("/custom/llvm/include/target/c++/v1/__config_site", error)

    def render(self, llvm_path: str, lld_path: str, user: str = "developer") -> str:
        captured = {}
        context = SimpleNamespace(
            name="mongo_apple_test",
            os=SimpleNamespace(
                environ={"LLVM_PATH": llvm_path, "LLD_PATH": lld_path, "USER": user}
            ),
            path=lambda value: SimpleNamespace(realpath=value, exists=True),
            read=lambda value: "#define _LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS\n",
            file=lambda path, content: None,
            report_progress=lambda message: None,
            template=lambda output, template, substitutions: captured.update(substitutions),
        )
        success, error = load_toolchain_functions()["_configure_oss_clang_toolchain"](context)
        self.assertTrue(success, error)
        result = Path(__file__).with_name("BUILD.tmpl").read_text()
        for key, value in captured.items():
            result = result.replace(key, value)
        return result

    def test_relative_paths_keep_the_repository_exec_prefix(self) -> None:
        tree = ast.parse(self.render("llvm/custom toolchain", 'lld/with"quote'))
        config = next(
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "mongo_apple_llvm_cc_toolchain_config"
        )
        values = {
            keyword.arg: eval(compile(ast.Expression(keyword.value), "BUILD.bazel", "eval"), {})
            for keyword in config.keywords
            if keyword.arg in {"tool_paths", "cxx_builtin_include_directories", "link_flags"}
        }
        self.assertEqual('lld/with"quote/bin/ld.lld', values["tool_paths"]["ld"])
        self.assertEqual(
            ['--ld-path=external/mongo_apple_test/lld/with"quote/bin/ld64.lld'],
            values["link_flags"],
        )
        self.assertIn(
            "external/mongo_apple_test/llvm/custom toolchain/include",
            values["cxx_builtin_include_directories"],
        )

    def test_paths_round_trip_in_generated_build(self) -> None:
        for suffix in [
            "ordinary",
            "with space",
            'with"quote',
            "with\\backslash",
            "with\nnewline",
            "with\ttab",
            "café",
        ]:
            with self.subTest(suffix=suffix):
                llvm_path = "/opt/llvm/" + suffix
                lld_path = "/opt/lld/" + suffix
                user = "developer" + suffix
                tree = ast.parse(self.render(llvm_path, lld_path, user))
                config = next(
                    node
                    for node in ast.walk(tree)
                    if isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Name)
                    and node.func.id == "mongo_apple_llvm_cc_toolchain_config"
                )
                values = {
                    keyword.arg: eval(
                        compile(ast.Expression(keyword.value), "BUILD.bazel", "eval"), {}
                    )
                    for keyword in config.keywords
                    if keyword.arg
                    in [
                        "tool_paths",
                        "cxx_builtin_include_directories",
                        "compile_flags",
                        "link_flags",
                    ]
                }
                self.assertEqual(values["tool_paths"]["gcc"], llvm_path + "/bin/clang")
                self.assertEqual(values["tool_paths"]["ld"], lld_path + "/bin/ld.lld")
                self.assertEqual(values["link_flags"], ["--ld-path=" + lld_path + "/bin/ld64.lld"])
                self.assertEqual(
                    values["compile_flags"],
                    ["-isystem", "external/mongo_apple_test/include/config_override"],
                )
                for name, value in values["tool_paths"].items():
                    self.assertTrue(
                        value.startswith((lld_path if name == "ld" else llvm_path) + "/bin/")
                    )
                includes = values["cxx_builtin_include_directories"]
                self.assertIn(llvm_path + "/include", includes)
                self.assertIn(llvm_path + "/lib", includes)
                self.assertIn("/Users/" + user + "/Library/", includes)


if __name__ == "__main__":
    unittest.main()
