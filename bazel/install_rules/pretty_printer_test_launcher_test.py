"""Tests for the path resolution used by the pretty-printer launcher."""

import ast
import os
import pathlib
import tempfile
import unittest
from unittest import mock

_TEMPLATE_RELATIVE_PATH = pathlib.Path("src/mongo/util/pretty_printer_test_launcher.py.in")


def _template_path() -> pathlib.Path:
    """Find the template in the source tree or Bazel's runfiles tree."""
    candidates = [_TEMPLATE_RELATIVE_PATH]
    runfiles_dir = os.environ.get("RUNFILES_DIR")
    if runfiles_dir:
        candidates.extend(
            [
                pathlib.Path(runfiles_dir) / "_main" / _TEMPLATE_RELATIVE_PATH,
                pathlib.Path(runfiles_dir) / _TEMPLATE_RELATIVE_PATH,
            ]
        )
    manifest_file = os.environ.get("RUNFILES_MANIFEST_FILE")
    if manifest_file:
        for line in pathlib.Path(manifest_file).read_text(encoding="utf-8").splitlines():
            manifest_path, separator, resolved_path = line.partition(" ")
            if separator and manifest_path.endswith("/" + str(_TEMPLATE_RELATIVE_PATH)):
                candidates.append(pathlib.Path(resolved_path))

    return next(path for path in candidates if path.exists())


def _load_resolver() -> object:
    """Load resolve_bazel_path from the launcher template without executing the launcher."""
    template = _template_path()
    source = (
        template.read_text(encoding="utf-8")
        .replace("@VERBOSE@", "False")
        .replace("@test_args@", "[]")
    )
    tree = ast.parse(source)
    resolver = next(node for node in tree.body if isinstance(node, ast.FunctionDef))
    namespace = {"glob": __import__("glob"), "os": os}
    exec(compile(ast.Module(body=[resolver], type_ignores=[]), str(template), "exec"), namespace)
    return namespace["resolve_bazel_path"]


class PrettyPrinterTestLauncherTest(unittest.TestCase):
    def test_resolves_gdb_from_the_installed_tree(self) -> None:
        resolve_bazel_path = _load_resolver()

        with tempfile.TemporaryDirectory() as temp_dir:
            gdb = pathlib.Path(temp_dir) / "bazel-bin/install/lib/gdb-toolchain/v5/bin/gdb"
            gdb.parent.mkdir(parents=True)
            gdb.touch()

            with mock.patch("os.getcwd", return_value=temp_dir):
                self.assertEqual(
                    gdb, pathlib.Path(resolve_bazel_path(str(gdb.relative_to(temp_dir))))
                )

    def test_resolves_gdb_from_a_bazel_runfiles_tree(self) -> None:
        resolve_bazel_path = _load_resolver()

        with tempfile.TemporaryDirectory() as temp_dir:
            runfiles_gdb = pathlib.Path(temp_dir) / "runfiles/_main/gdb-toolchain/v5/bin/gdb"
            runfiles_gdb.parent.mkdir(parents=True)
            runfiles_gdb.touch()

            with mock.patch.dict(
                os.environ,
                {
                    "RUNFILES_DIR": str(pathlib.Path(temp_dir) / "runfiles"),
                    "TEST_WORKSPACE": "_main",
                },
                clear=False,
            ):
                with mock.patch("os.getcwd", return_value=temp_dir):
                    self.assertEqual(
                        runfiles_gdb,
                        pathlib.Path(
                            resolve_bazel_path("bazel-bin/install/lib/gdb-toolchain/v5/bin/gdb")
                        ),
                    )

    def test_resolves_gdb_from_a_manifest_tree_entry(self) -> None:
        resolve_bazel_path = _load_resolver()

        with tempfile.TemporaryDirectory() as temp_dir:
            toolchain = pathlib.Path(temp_dir) / "gdb-toolchain"
            manifest_gdb = toolchain / "v5/bin/gdb"
            manifest_gdb.parent.mkdir(parents=True)
            manifest_gdb.touch()
            manifest = pathlib.Path(temp_dir) / "MANIFEST"
            manifest.write_text(f"_main/gdb-toolchain {toolchain}\n", encoding="utf-8")

            with mock.patch.dict(
                os.environ,
                {"RUNFILES_MANIFEST_FILE": str(manifest)},
                clear=True,
            ):
                self.assertEqual(
                    manifest_gdb,
                    pathlib.Path(
                        resolve_bazel_path("bazel-bin/install/lib/gdb-toolchain/v5/bin/gdb")
                    ),
                )


if __name__ == "__main__":
    unittest.main()
