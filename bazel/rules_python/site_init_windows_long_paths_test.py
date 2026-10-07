"""Tests the rules_python site-init long-path patch."""

import ast
import ntpath
import sys
import types
import unittest
from pathlib import Path

_SITE_INIT_SOURCE = Path(sys.argv.pop(1))


def _load_path_helper():
    module = ast.parse(_SITE_INIT_SOURCE.read_text(encoding="utf-8"))
    function = next(
        node
        for node in module.body
        if isinstance(node, ast.FunctionDef) and node.name == "_force_windows_long_path"
    )
    namespace = {"os": types.SimpleNamespace(path=ntpath)}
    exec(
        compile(ast.Module([function], type_ignores=[]), str(_SITE_INIT_SOURCE), "exec"), namespace
    )
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

    def test_site_init_forces_only_import_roots(self):
        source = _SITE_INIT_SOURCE.read_text(encoding="utf-8")

        self.assertIn(
            'if not force and win32_version and win32_version >= "10.0.14393":',
            source,
        )
        self.assertIn('force=reason == "imports-strs"', source)


if __name__ == "__main__":
    unittest.main()
