"""Exercise the environment seen by pip and its build subprocesses."""

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import python_build_environment


class PythonBuildEnvironmentTest(unittest.TestCase):
    def test_absolute_paths_and_cleanup_on_success_and_failure(self) -> None:
        for exit_code in (0, 1):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as directory:
                action_dir = Path(directory).resolve()
                original_cwd = Path.cwd()
                original_tempdir = tempfile.tempdir
                captured: dict[str, str] = {}

                def backend(module: str, **kwargs: object) -> None:
                    self.assertEqual("pip", module)
                    self.assertEqual(["pip", "wheel", "avro-python3"], sys.argv)
                    self.assertEqual("__main__", kwargs["run_name"])
                    for name in (
                        "HOME",
                        "USERPROFILE",
                        "CARGO_HOME",
                        "XDG_CACHE_HOME",
                        "LOCALAPPDATA",
                        "APPDATA",
                        "TMPDIR",
                        "TEMP",
                        "TMP",
                    ):
                        captured[name] = os.environ[name]
                        path = Path(captured[name])
                        self.assertTrue(path.is_absolute(), name)
                        self.assertTrue(path.is_relative_to(action_dir), name)
                        self.assertTrue(path.is_dir(), name)
                    home = Path(captured["HOME"])
                    os.chdir(home)
                    # HOME must remain valid after PEP 517 changes into the source directory.
                    self.assertEqual(home, Path("~").expanduser())
                    self.assertEqual(home / ".cargo", Path(captured["CARGO_HOME"]))
                    with tempfile.NamedTemporaryFile() as scratch:
                        self.assertTrue(Path(scratch.name).is_relative_to(action_dir))
                    (Path(captured["XDG_CACHE_HOME"]) / "puccinialin").mkdir()
                    raise SystemExit(exit_code)

                try:
                    os.chdir(action_dir)
                    with (
                        mock.patch.dict(os.environ, {"HOME": ".", "CARGO_HOME": ".cargo"}),
                        mock.patch.object(
                            sys, "argv", ["runner.py", "pip", "wheel", "avro-python3"]
                        ),
                        mock.patch.object(
                            python_build_environment.runpy, "run_module", side_effect=backend
                        ),
                    ):
                        with self.assertRaises(SystemExit) as raised:
                            python_build_environment.main()
                        self.assertEqual(exit_code, raised.exception.code)
                        self.assertEqual(".", os.environ["HOME"])
                        self.assertEqual(".cargo", os.environ["CARGO_HOME"])
                        self.assertEqual(action_dir, Path.cwd())
                        self.assertEqual(original_tempdir, tempfile.tempdir)
                    self.assertFalse(Path(captured["HOME"]).exists())
                    self.assertEqual([], list(action_dir.iterdir()))
                finally:
                    os.chdir(original_cwd)


if __name__ == "__main__":
    unittest.main()
