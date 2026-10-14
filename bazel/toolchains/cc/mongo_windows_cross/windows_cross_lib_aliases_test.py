from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "windows_cross_lib_aliases",
    Path(__file__).with_name("windows_cross_lib_aliases.py"),
)
assert _SPEC is not None and _SPEC.loader is not None
_MODULE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MODULE)
create_lib_aliases = _MODULE.create_lib_aliases


class WindowsCrossLibAliasesTest(unittest.TestCase):
    def test_creates_uppercase_library_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir)
            source_dir = repo / "sysroot" / "msvc" / "lib" / "x64"
            source_dir.mkdir(parents=True)
            (source_dir / "msvcrtd.lib").write_bytes(b"msvcrtd")
            (source_dir / "oldnames.lib").write_bytes(b"oldnames")

            create_lib_aliases(repo)

            alias_dir = repo / "lib_aliases" / "upper_base_lower_extension" / "msvc" / "lib" / "x64"
            self.assertEqual((alias_dir / "MSVCRTD.lib").read_bytes(), b"msvcrtd")
            self.assertEqual((alias_dir / "OLDNAMES.lib").read_bytes(), b"oldnames")


if __name__ == "__main__":
    unittest.main()
