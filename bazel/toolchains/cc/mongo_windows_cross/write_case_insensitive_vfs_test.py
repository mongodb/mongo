from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "write_case_insensitive_vfs", Path(__file__).with_name("write_case_insensitive_vfs.py")
)
assert _SPEC is not None and _SPEC.loader is not None
_MODULE = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_MODULE)
generate_overlay = _MODULE.generate_overlay


class WriteCaseInsensitiveVfsTest(unittest.TestCase):
    def test_generates_case_and_include_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir)
            include_dir = repo / "sysroot" / "winsdk" / "include" / "um"
            include_dir.mkdir(parents=True)
            (include_dir / "Windows.h").write_text("#include <WinSock2.h>\n", encoding="utf-8")
            (include_dir / "winsock2.h").write_text("", encoding="utf-8")

            overlay = generate_overlay(repo, "external/toolchain", ["winsdk/include/um"])

        contents = overlay["roots"][0]["contents"]
        aliases = {entry["name"]: entry["external-contents"] for entry in contents}
        self.assertEqual(
            aliases["windows.h"],
            "external/toolchain/sysroot/winsdk/include/um/Windows.h",
        )
        self.assertEqual(
            aliases["WinSock2.h"],
            "external/toolchain/sysroot/winsdk/include/um/winsock2.h",
        )


if __name__ == "__main__":
    unittest.main()
