"""Check migration of wrapper configuration away from host architecture naming."""

import pathlib
import tempfile
import unittest
from unittest import mock

from bazel.wrapper_hook import write_wrapper_hook_bazelrc


class WrapperHookBazelrcTest(unittest.TestCase):
    def test_removes_legacy_architecture_and_preserves_workspace_status(self) -> None:
        for platform, python in (
            ("linux", "/venv/bin/python"),
            ("win32", r"C:\venv\Scripts\python.exe"),
        ):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                config = root / ".bazelrc.wrapper_hook"
                config.write_text("common --define=MONGO_ARCH=aarch64\n", encoding="utf-8")
                with (
                    mock.patch.object(
                        write_wrapper_hook_bazelrc,
                        "__file__",
                        str(root / "bazel" / "wrapper_hook" / "write_wrapper_hook_bazelrc.py"),
                    ),
                    mock.patch.object(write_wrapper_hook_bazelrc.sys, "platform", platform),
                    mock.patch.object(write_wrapper_hook_bazelrc.sys, "executable", python),
                ):
                    write_wrapper_hook_bazelrc.write_wrapper_hook_bazelrc([])

                contents = config.read_text(encoding="utf-8")
                self.assertNotIn("MONGO_ARCH", contents)
                self.assertIn("--workspace_status_command=", contents)
                escaped_python = python.replace("\\", "\\\\") if platform == "win32" else python
                self.assertIn(escaped_python, contents)
                self.assertIn("workspace_status.py", contents)


if __name__ == "__main__":
    unittest.main()
