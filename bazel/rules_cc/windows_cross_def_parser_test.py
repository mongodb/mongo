"""Validate rules_cc's parser selection for a Windows Bazel client."""

import ast
import sys
import unittest
from pathlib import Path

SEMANTICS = next(Path(arg) for arg in sys.argv[1:] if arg.endswith("/semantics.bzl"))
sys.argv[1:] = []


class WindowsCrossDefParserTest(unittest.TestCase):
    def test_windows_client_uses_native_parser_for_linux_exec_platforms(self) -> None:
        tree = ast.parse(SEMANTICS.read_text())
        function = next(
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name == "_def_parser_computed_default"
        )
        namespace = {"Label": str}
        exec(
            compile(ast.Module(body=[function], type_ignores=[]), str(SEMANTICS), "exec"), namespace
        )
        parser = namespace[function.name]
        self.assertEqual(parser("gtest", []), "@@//bazel/rules_cc:def_parser")
        self.assertIsNone(parser("def_parser", []))
        self.assertIsNone(parser("malloc", ["__DONT_DEPEND_ON_DEF_PARSER__"]))

        build = ast.parse(Path(__file__).with_name("BUILD.bazel").read_text())
        alias = next(
            node.value
            for node in build.body
            if isinstance(node, ast.Expr)
            and isinstance(node.value, ast.Call)
            and isinstance(node.value.func, ast.Name)
            and node.value.func.id == "alias"
        )
        actual = next(keyword.value for keyword in alias.keywords if keyword.arg == "actual")
        choices = ast.literal_eval(actual.args[0])
        self.assertEqual(
            choices["//bazel/config:windows_cross_local_container_actions_enabled"],
            "@bazel_tools//tools/def_parser:def_parser.exe",
        )
        self.assertEqual(
            choices["//conditions:default"], "@bazel_tools//tools/def_parser:def_parser"
        )


if __name__ == "__main__":
    unittest.main()
