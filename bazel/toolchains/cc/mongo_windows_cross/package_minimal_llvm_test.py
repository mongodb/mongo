"""Regression coverage for regular-file LLVM archives used by Windows clients."""

from __future__ import annotations

import io
import tarfile
import tempfile
import unittest
from pathlib import Path

from bazel.toolchains.cc.mongo_windows_cross import package_minimal_llvm as package


class PackageMinimalLlvmTest(unittest.TestCase):
    def test_multicall_tools_are_stored_once_under_their_real_names(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "bin").mkdir()
            for name in (
                "clang-19",
                "lld",
                "llvm-ar",
                "llvm-rc",
                "llvm-symbolizer",
                "llvm-pdbutil",
            ):
                (root / "bin" / name).write_bytes(name.encode())
            (root / "bin" / "clang").symlink_to("clang-19")
            headers = root / "lib" / "clang" / "19" / "include"
            headers.mkdir(parents=True)
            (headers / "stddef.h").write_bytes(b"header")

            files = package.collect_files(root, "19")

            self.assertNotIn("bin/clang", files)
            self.assertEqual(files["bin/clang-19"], (root / "bin" / "clang-19").resolve())
            self.assertEqual(len(files), 7)
            self.assertTrue(
                all(path.is_file() and not path.is_symlink() for path in files.values())
            )

    def test_tool_links_must_stay_inside_the_release(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "llvm"
            (root / "bin").mkdir(parents=True)
            outside = Path(temp_dir) / "outside"
            outside.write_bytes(b"tool")
            (root / "bin" / "clang").symlink_to(outside)
            with self.assertRaisesRegex(SystemExit, "inside the release"):
                package.resolve_tool(root, "clang")

    def test_archive_members_have_stable_metadata(self) -> None:
        content = io.BytesIO()
        with tarfile.open(fileobj=content, mode="w") as archive:
            package.add_file(archive, "llvm/LICENSE.TXT", b"license", None)
        content.seek(0)
        with tarfile.open(fileobj=content, mode="r") as archive:
            member = archive.getmember("llvm/LICENSE.TXT")
            self.assertTrue(member.isfile())
            self.assertEqual((member.mtime, member.uid, member.gid, member.mode), (0, 0, 0, 0o644))
            self.assertEqual(archive.extractfile(member).read(), b"license")


if __name__ == "__main__":
    unittest.main()
