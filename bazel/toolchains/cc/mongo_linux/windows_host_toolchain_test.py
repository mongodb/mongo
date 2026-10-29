"""Regression coverage for a Windows client building Linux execution tools."""

import ast
import unittest
from collections.abc import Callable
from pathlib import Path
from types import SimpleNamespace
from typing import Any


def load_function(filename: str, name: str) -> Callable[..., Any]:
    path = Path(__file__).with_name(filename)
    tree = ast.parse(path.read_text())
    node = next(
        node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == name
    )
    namespace = {}
    exec(compile(ast.Module(body=[node], type_ignores=[]), str(path), "exec"), namespace)
    return namespace[name]


class WindowsHostToolchainTest(unittest.TestCase):
    def test_all_shared_override_callers_track_the_repository_environment(self):
        root = Path(__file__).parents[3]
        source = ast.parse((root / "utils.bzl").read_text())
        assignments = [
            node
            for node in source.body
            if isinstance(node, ast.Assign)
            and any(
                isinstance(target, ast.Name)
                and (target.id.startswith("_TOOLCHAIN_") or target.id == "TOOLCHAIN_OVERRIDE_ENVS")
                for target in node.targets
            )
        ]
        namespace = {}
        exec(compile(ast.Module(body=assignments, type_ignores=[]), "utils.bzl", "exec"), namespace)
        expected = set(namespace["TOOLCHAIN_OVERRIDE_ENVS"])
        self.assertEqual(
            expected,
            {
                "MONGO_BAZEL_TOOLCHAIN_OS",
                "MONGO_BAZEL_TOOLCHAIN_ARCH",
                "MONGO_BAZEL_TOOLCHAIN_DISTRO",
            },
        )
        for filename, rule in (
            ("mongo_toolchain.bzl", "toolchain_download"),
            ("mongo_gdb.bzl", "gdb_v5_download"),
        ):
            tree = ast.parse(Path(__file__).with_name(filename).read_text())
            declaration = next(
                node.value
                for node in tree.body
                if isinstance(node, ast.Assign) and node.targets[0].id == rule
            )
            environ = next(
                keyword.value for keyword in declaration.keywords if keyword.arg == "environ"
            )
            tracked = eval(
                compile(ast.Expression(environ), filename, "eval"),
                {
                    **namespace,
                    "SKIP_TOOLCHAIN_ENVIRONMENT_VARIABLE": "skip",
                    "SYSROOT_ENV_VAR": "sysroot",
                    "LINUX_CROSS_TOOLCHAIN_ENV_VAR": "cross",
                },
            )
            self.assertTrue(expected.issubset(tracked), filename)

    def test_dependency_aliases_match_windows_parser_without_hiding_user_headers(self) -> None:
        aliases = load_function("mongo_toolchain_tool_paths.bzl", "windows_builtin_include_aliases")
        includes = ["/usr/include", "/opt/openssl/include", "external/toolchain/include"]
        root = "C:/bazel-linux"
        result = aliases(includes, root)
        self.assertEqual(
            result, ["C:/bazel-linux/usr/include", "C:/bazel-linux/opt/openssl/include"]
        )
        # Match the Windows DependencySet translation for an absolute Linux path.
        self.assertTrue(
            any((root + "/usr/include/features.h").startswith(path + "/") for path in result)
        )
        for header in [
            "C:/bazel-linux/workspace/project.h",
            "C:/bazel-linux/usr/include-other/header.h",
            "external/dependency/header.h",
        ]:
            self.assertFalse(any(header.startswith(path + "/") for path in result))
        self.assertEqual(
            includes, ["/usr/include", "/opt/openssl/include", "external/toolchain/include"]
        )
        self.assertEqual(aliases(includes, ""), [])

    def test_dependency_root_is_only_applied_on_windows_clients(self) -> None:
        substitutions = load_function("mongo_toolchain.bzl", "_windows_host_substitutions")
        for host in ("windows", "linux", "mac os x"):
            with self.subTest(host=host):
                values = substitutions(
                    SimpleNamespace(
                        os=SimpleNamespace(
                            name=host,
                            environ={"MONGO_BAZEL_WINDOWS_UNIX_ROOT": "C:/bazel-linux"},
                        )
                    )
                )
                self.assertEqual(
                    ast.literal_eval(values["{windows_unix_root}"]),
                    "C:/bazel-linux" if host == "windows" else "",
                )


if __name__ == "__main__":
    unittest.main()
