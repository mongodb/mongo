"""Tests the rules_python site-init long-path patch."""

import ast
import ntpath
import os
import types
import unittest
from pathlib import Path

_PATCH_NAME = "site_init_windows_long_paths.patch"


def _find_patch() -> Path:
    direct_path = Path(__file__).with_name(_PATCH_NAME)
    if direct_path.is_file():
        return direct_path

    runfiles_dir = os.environ.get("RUNFILES_DIR")
    if runfiles_dir:
        workspace = os.environ.get("TEST_WORKSPACE", "_main")
        runfiles_path = Path(runfiles_dir) / workspace / "bazel/rules_python" / _PATCH_NAME
        if runfiles_path.is_file():
            return runfiles_path

    manifest_path = os.environ.get("RUNFILES_MANIFEST_FILE")
    if manifest_path:
        for line in Path(manifest_path).read_text(encoding="utf-8").splitlines():
            key, _, value = line.partition(" ")
            if key.endswith(f"/bazel/rules_python/{_PATCH_NAME}"):
                return Path(value)

    raise FileNotFoundError(_PATCH_NAME)


def _load_path_helper():
    patch_lines = _find_patch().read_text(encoding="utf-8").splitlines()
    start = patch_lines.index("+def _force_windows_long_path(path):")
    added_lines = []
    for line in patch_lines[start:]:
        if not line.startswith("+"):
            break
        added_lines.append(line[1:])

    namespace = {"os": types.SimpleNamespace(path=ntpath)}
    tree = ast.parse("\n".join(added_lines))
    exec(compile(tree, str(_find_patch()), "exec"), namespace)  # noqa: S102
    return namespace["_force_windows_long_path"]


class SiteInitWindowsLongPathsTest(unittest.TestCase):
    def test_drive_paths_use_extended_length_prefix(self):
        force_long_path = _load_path_helper()

        self.assertEqual(
            r"\\?\C:\runfiles\repo\package",
            force_long_path(r"C:\runfiles\repo\package"),
        )

    def test_unc_paths_use_unc_extended_length_prefix(self):
        force_long_path = _load_path_helper()

        self.assertEqual(
            r"\\?\UNC\server\share\package",
            force_long_path(r"\\server\share\package"),
        )

    def test_already_prefixed_paths_are_unchanged(self):
        force_long_path = _load_path_helper()
        path = r"\\?\C:\runfiles\repo\package"

        self.assertEqual(path, force_long_path(path))

    def test_patch_forces_only_import_roots(self):
        patch = _find_patch().read_text(encoding="utf-8")

        self.assertIn(
            'if not force and win32_version and win32_version >= "10.0.14393":',
            patch,
        )
        self.assertIn('force=reason == "imports-strs"', patch)


if __name__ == "__main__":
    unittest.main()
