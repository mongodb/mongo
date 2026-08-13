from __future__ import annotations

import importlib.util
import pathlib
import types
import unittest
from unittest import mock


def _load_gpg_export_armored_key() -> types.ModuleType:
    path = pathlib.Path(__file__).with_name("gpg_export_armored_key.py")
    spec = importlib.util.spec_from_file_location("gpg_export_armored_key", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gpg_export_armored_key = _load_gpg_export_armored_key()


class GpgExportArmoredKeyTest(unittest.TestCase):
    def test_gpg_home_uses_action_tmpdir(self) -> None:
        action_tmpdir = "/writable/action-tmp"
        with (
            mock.patch.dict(
                gpg_export_armored_key.os.environ,
                {"TMPDIR": action_tmpdir},
                clear=True,
            ),
            mock.patch.object(
                gpg_export_armored_key.tempfile,
                "mkdtemp",
                return_value=f"{action_tmpdir}/mongo-gpg-test",
            ) as mkdtemp,
            mock.patch.object(gpg_export_armored_key.os, "chmod") as chmod,
        ):
            result = gpg_export_armored_key._create_gpg_home()

        self.assertEqual(pathlib.Path(action_tmpdir) / "mongo-gpg-test", result)
        mkdtemp.assert_called_once_with(prefix="mongo-gpg-", dir=action_tmpdir)
        chmod.assert_called_once_with(f"{action_tmpdir}/mongo-gpg-test", 0o700)

    def test_run_discards_stderr_when_stdout_is_discarded(self) -> None:
        with mock.patch.object(gpg_export_armored_key.subprocess, "run") as run:
            gpg_export_armored_key._run(["gpg-agent"])

        self.assertEqual(gpg_export_armored_key.os.devnull, run.call_args.kwargs["stdout"].name)
        self.assertEqual(gpg_export_armored_key.os.devnull, run.call_args.kwargs["stderr"].name)


if __name__ == "__main__":
    unittest.main()
