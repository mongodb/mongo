"""Test the actual patched wheel builder without importing its build tool dependencies."""

from __future__ import annotations

import ast
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

_BUILDER_SOURCE = Path(sys.argv.pop(1))


def _load_environment_functions() -> dict[str, object]:
    module = ast.parse(_BUILDER_SOURCE.read_text(encoding="utf-8"))
    nodes = [
        node
        for node in module.body
        if isinstance(node, ast.FunctionDef)
        and node.name
        in {"_sandbox_environment", "main_wrapper", "init_build_env_vars", "set_or_append"}
    ]
    namespace: dict[str, object] = {
        "Path": Path,
        "os": os,
        "tempfile": tempfile,
        "shutil": shutil,
        "json": json,
        "get_default_build_env_vars": lambda _: os.environ.copy(),
        "replace_path_placeholders": lambda value, *_: value,
    }
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "wheel_builder", "exec"), namespace)
    return namespace


class WheelBuildEnvironmentTest(unittest.TestCase):
    def test_runtime_paths_and_cleanup(self) -> None:
        for failure in (False, True):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                action_dir = Path(directory).resolve()
                original_cwd = Path.cwd()
                previous_tempdir = tempfile.tempdir
                namespace = _load_environment_functions()
                captured: dict[str, str] = {}

                def backend(args: object, build_dir: Path, debug: bool) -> None:
                    self.assertFalse(debug)
                    self.assertEqual(action_dir, build_dir.parent)
                    captured.update(namespace["_sandbox_environment"](build_dir))
                    for name, value in captured.items():
                        path = Path(value)
                        self.assertTrue(path.is_absolute(), name)
                        self.assertTrue(path.is_relative_to(build_dir), name)
                        self.assertEqual(value, os.environ[name])
                    with tempfile.NamedTemporaryFile() as scratch:
                        self.assertTrue(Path(scratch.name).is_relative_to(build_dir))
                    source_dir = build_dir / "sdist"
                    source_dir.mkdir()
                    os.chdir(source_dir)
                    self.assertEqual(Path(captured["HOME"]), Path("~").expanduser())
                    # build_env values must not concatenate with, or override, resolved paths.
                    config = build_dir / "build_env.json"
                    config.write_text(
                        json.dumps({name: "." for name in captured}), encoding="utf-8"
                    )
                    environment = namespace["init_build_env_vars"](
                        SimpleNamespace(build_env=config),
                        Path(".."),
                        [],
                        [],
                        [],
                        Path("../bazel-execroot"),
                    )
                    for name, value in captured.items():
                        self.assertEqual(value, environment[name], name)
                    (Path(captured["XDG_CACHE_HOME"]) / "puccinialin").mkdir()
                    if failure:
                        raise RuntimeError("backend failed")

                namespace["main"] = backend
                try:
                    os.chdir(action_dir)
                    with mock.patch.dict(os.environ, {"HOME": ".", "CARGO_HOME": ".cargo"}):
                        os.environ.pop("RULES_PYCROSS_DEBUG", None)
                        if failure:
                            with self.assertRaisesRegex(RuntimeError, "backend failed"):
                                namespace["main_wrapper"](None)
                        else:
                            namespace["main_wrapper"](None)
                        self.assertEqual(".", os.environ["HOME"])
                        self.assertEqual(".cargo", os.environ["CARGO_HOME"])
                        self.assertEqual(previous_tempdir, tempfile.tempdir)
                        self.assertEqual(action_dir, Path.cwd())
                    self.assertFalse(Path(captured["HOME"]).exists())
                    self.assertEqual([], list(action_dir.iterdir()))
                finally:
                    os.chdir(original_cwd)

    def test_debug_keeps_state_inside_action(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            action_dir = Path(directory).resolve()
            original_cwd = Path.cwd()
            namespace = _load_environment_functions()
            build_dirs: list[Path] = []

            def backend(args: object, build_dir: Path, debug: bool) -> None:
                self.assertTrue(debug)
                build_dirs.append(build_dir)

            namespace["main"] = backend
            try:
                os.chdir(action_dir)
                with mock.patch.dict(os.environ, {"RULES_PYCROSS_DEBUG": "1"}):
                    namespace["main_wrapper"](None)
                self.assertTrue(build_dirs[0].is_dir())
                self.assertEqual(action_dir, build_dirs[0].parent)
            finally:
                os.chdir(original_cwd)


if __name__ == "__main__":
    unittest.main()
