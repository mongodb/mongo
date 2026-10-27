"""Regression tests for hydrating Python execution toolchains on Windows."""

import ast
import shutil
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from types import SimpleNamespace
from typing import Any


class RepositoryPath:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.basename = path.name

    def readdir(self) -> list["RepositoryPath"]:
        return [RepositoryPath(path) for path in self.path.iterdir()]


class RepositoryContext:
    def __init__(self, root: Path, host_os: str, toolchain_os: str) -> None:
        self.root = root
        self.os = SimpleNamespace(
            name=host_os,
            arch="x86_64",
            environ={
                "MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON": "1",
                "MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH": "x86_64",
            },
        )
        self.attr = SimpleNamespace(
            os=toolchain_os,
            arch="amd64",
            urls=["fixture"],
            sha256="fixture",
            interpreter_path="dist/python.exe" if toolchain_os == "windows" else "dist/bin/python3",
            build_tpl=Path(__file__).with_name("python_toolchain.BUILD.tmpl"),
        )

    def path(self, name: str) -> RepositoryPath:
        return RepositoryPath(self.root / name)

    def file(self, name: str, content: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def delete(self, name: str) -> None:
        shutil.rmtree(self.root / name)

    def template(self, name: str, template: Path, substitutions: dict[str, str]) -> None:
        content = template.read_text()
        for old, new in substitutions.items():
            content = content.replace(old, new)
        self.file(name, content)

    def report_progress(self, message: str) -> None:
        pass

    def execute(self, args: list[str]) -> None:
        pass


def load_download() -> Callable[[RepositoryContext], Any]:
    source = Path(__file__).with_name("python_toolchain.bzl")
    tree = ast.parse(source.read_text())
    function = next(
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "_py_download"
    )
    namespace = {
        "_DOWNLOAD_CROSS_LINUX_PYTHON_ENV": "MONGO_BAZEL_DOWNLOAD_CROSS_LINUX_PYTHON",
        "_CROSS_LINUX_PYTHON_ARCH_ENV": "MONGO_BAZEL_CROSS_LINUX_PYTHON_ARCH",
        "OS_TO_PLATFORM_MAP": {os: "@platforms//os:" + os for os in ("linux", "macos", "windows")},
        "ARCH_TO_PLATFORM_MAP": {"amd64": "@platforms//cpu:x86_64"},
        "retry_download_and_extract": lambda **kwargs: None,
        "write_python_pyc_cache_prefix_customization": lambda ctx, path: ctx.file(
            path, "# customization\n"
        ),
    }
    exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), "exec"), namespace)
    return namespace["_py_download"]


class PythonToolchainTest(unittest.TestCase):
    def test_cross_linux_python_removes_case_colliding_terminfo_on_windows(self) -> None:
        download = load_download()
        for host_os, toolchain_os in (
            ("windows", "linux"),
            ("linux", "linux"),
            ("mac os x", "linux"),
            ("windows", "windows"),
        ):
            with (
                self.subTest(host_os=host_os, toolchain_os=toolchain_os),
                tempfile.TemporaryDirectory() as tmp,
            ):
                root = Path(tmp)
                ctx = RepositoryContext(root, host_os, toolchain_os)
                ctx.file("dist/bin/python3.13", "interpreter\n")
                ctx.file("dist/lib/python3.13/os.py", "stdlib\n")
                if toolchain_os == "windows":
                    ctx.file("dist/python.exe", "interpreter\n")
                ctx.file("dist/python313.dll", "runtime DLL\n")
                ctx.file("dist/DLLs/_socket.pyd", "extension\n")
                terminfo = root / "dist/share/terminfo/n"
                terminfo.mkdir(parents=True)
                # Case-only aliases in the Linux archive become self-referential on Windows.
                (terminfo / "ncr260vt300wpp").symlink_to("ncr260vt300wpp")
                download(ctx)
                self.assertEqual(
                    (root / "dist/share/terminfo").exists(),
                    host_os != "windows" or toolchain_os != "linux",
                )
                for file in (
                    "dist/bin/python3.13",
                    "dist/lib/python3.13/os.py",
                    "dist/python313.dll",
                    "dist/DLLs/_socket.pyd",
                    "BUILD.bazel",
                ):
                    self.assertTrue((root / file).is_file(), file)


if __name__ == "__main__":
    unittest.main()
