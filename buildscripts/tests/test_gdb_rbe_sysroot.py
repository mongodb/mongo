from __future__ import annotations

import os
import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock

from buildscripts import gdb_rbe_sysroot

ET_DYN = 3

# gdb_rbe_sysroot.py only runs on Linux spawn hosts, and these tests write filenames and shell
# script stubs that are invalid on Windows.
linux_only = unittest.skipUnless(
    sys.platform.startswith("linux"), "gdb_rbe_sysroot.py is Linux-only"
)

# Captured from `eu-unstrip -n --core` on a core produced inside the amazon_linux_2023 RBE container.
UNSTRIP_OUTPUT = """\
0xaaaae7c70000+0x179000 3e4dfab7d3b777b487666b3a058737bf7e119ad6@0xaaaae7c70368 - - /usr/bin/coreutils
0xffff99754000+0x1ce000 83049cea851867ce99cf7db3b4b331ea9011a7f4@0xffff99754380 - - /usr/lib64/libc.so.6
0xfffff7ffb000+0x1000 -@0xfffff7ffb278 . - linux-vdso.so.1
"""


def _pad(data: bytes) -> bytes:
    return data + b"\0" * (-len(data) % 4)


def _note(name: bytes, note_type: int, desc: bytes, endian: str) -> bytes:
    name += b"\0"
    return struct.pack(endian + "III", len(name), len(desc), note_type) + _pad(name) + _pad(desc)


def _build_id_note(build_id: str, endian: str = "<") -> bytes:
    return _note(b"GNU", gdb_rbe_sysroot.NT_GNU_BUILD_ID, bytes.fromhex(build_id), endian)


def _nt_file_note(paths: list[str], is64: bool = True, endian: str = "<") -> bytes:
    word = "Q" if is64 else "I"
    desc = struct.pack(endian + word * 2, len(paths), 4096)
    for i in range(len(paths)):
        desc += struct.pack(endian + word * 3, 0x1000 * i, 0x1000 * (i + 1), 0)
    desc += b"".join(os.fsencode(p) + b"\0" for p in paths)
    return _note(b"CORE", gdb_rbe_sysroot.NT_FILE, desc, endian)


def _write_elf(
    path: pathlib.Path, e_type: int, notes: bytes, is64: bool = True, endian: str = "<"
) -> pathlib.Path:
    ident = b"\x7fELF" + bytes([2 if is64 else 1, 1 if endian == "<" else 2, 1]) + b"\0" * 9
    if is64:
        ehdr_size, phdr_size = 64, 56
        header = struct.pack(
            endian + "HHIQQQIHHHHHH",
            e_type,
            0,
            1,
            0,
            ehdr_size,
            0,
            0,
            ehdr_size,
            phdr_size,
            1,
            0,
            0,
            0,
        )
        phdr = struct.pack(
            endian + "IIQQQQQQ",
            gdb_rbe_sysroot.PT_NOTE,
            0,
            ehdr_size + phdr_size,
            0,
            0,
            len(notes),
            0,
            4,
        )
    else:
        ehdr_size, phdr_size = 52, 32
        header = struct.pack(
            endian + "HHIIIIIHHHHHH",
            e_type,
            0,
            1,
            0,
            ehdr_size,
            0,
            0,
            ehdr_size,
            phdr_size,
            1,
            0,
            0,
            0,
        )
        phdr = struct.pack(
            endian + "IIIIIIII",
            gdb_rbe_sysroot.PT_NOTE,
            ehdr_size + phdr_size,
            0,
            0,
            len(notes),
            0,
            0,
            4,
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(ident + header + phdr + notes)
    return path


@linux_only
class ElfParsingTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_file_build_id_for_each_class_and_byte_order(self) -> None:
        for is64 in (True, False):
            for endian in ("<", ">"):
                with self.subTest(is64=is64, endian=endian):
                    lib = _write_elf(
                        self.root / f"lib{is64}{endian}.so",
                        ET_DYN,
                        _build_id_note("deadbeef01", endian),
                        is64=is64,
                        endian=endian,
                    )
                    self.assertEqual(gdb_rbe_sysroot.file_build_id(str(lib)), "deadbeef01")

    def test_file_build_id_ignores_other_notes(self) -> None:
        notes = _note(b"GNU", 5, b"\0" * 8, "<") + _build_id_note("abcd")
        lib = _write_elf(self.root / "lib.so", ET_DYN, notes)
        self.assertEqual(gdb_rbe_sysroot.file_build_id(str(lib)), "abcd")

    def test_file_build_id_without_elf(self) -> None:
        text = self.root / "script.sh"
        text.write_text("#!/bin/sh\n")
        self.assertIsNone(gdb_rbe_sysroot.file_build_id(str(text)))
        self.assertIsNone(gdb_rbe_sysroot.file_build_id(str(self.root / "missing.so")))

    def test_is_core_file(self) -> None:
        core = _write_elf(self.root / "dump.core", gdb_rbe_sysroot.ET_CORE, b"")
        lib = _write_elf(self.root / "lib.so", ET_DYN, b"")
        self.assertTrue(gdb_rbe_sysroot.is_core_file(str(core)))
        self.assertFalse(gdb_rbe_sysroot.is_core_file(str(lib)))
        self.assertFalse(gdb_rbe_sysroot.is_core_file(str(self.root / "missing.core")))

    def test_core_mapped_files_deduplicates_in_order(self) -> None:
        paths = ["/usr/bin/mongod", "/usr/lib64/libc.so.6", "/usr/lib64/libc.so.6"]
        for is64 in (True, False):
            for endian in ("<", ">"):
                with self.subTest(is64=is64, endian=endian):
                    core = _write_elf(
                        self.root / f"dump{is64}{endian}.core",
                        gdb_rbe_sysroot.ET_CORE,
                        _note(b"CORE", 1, b"\0" * 16, endian)
                        + _nt_file_note(paths, is64=is64, endian=endian),
                        is64=is64,
                        endian=endian,
                    )
                    self.assertEqual(
                        gdb_rbe_sysroot.core_mapped_files(str(core)),
                        ["/usr/bin/mongod", "/usr/lib64/libc.so.6"],
                    )

    def test_parse_unstrip_build_ids(self) -> None:
        self.assertEqual(
            gdb_rbe_sysroot.parse_unstrip_build_ids(UNSTRIP_OUTPUT),
            {
                "3e4dfab7d3b777b487666b3a058737bf7e119ad6",
                "83049cea851867ce99cf7db3b4b331ea9011a7f4",
            },
        )

    def test_core_build_ids_with_undecodable_module_path(self) -> None:
        eu_unstrip = self.root / "eu-unstrip"
        eu_unstrip.write_bytes(
            b"#!/bin/sh\nprintf '0x1+0x2 abcd@0x3 . - /usr/lib64/\\377bad.so\\n'\n"
        )
        eu_unstrip.chmod(0o755)
        self.assertEqual(gdb_rbe_sysroot.core_build_ids("dump.core", str(eu_unstrip)), {"abcd"})


@linux_only
class CoreUsesRbeSysrootTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        self.sysroot = self.root / "rbe_sysroot"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _uses_sysroot(self, libs: dict[str, tuple[str, str | None]], loaded: set[str]) -> bool:
        """`libs` maps a library name to its (host, sysroot) build-ids; None means not in sysroot."""
        host_paths = []
        for name, (host_id, sysroot_id) in libs.items():
            host_lib = _write_elf(
                self.root / "host" / "lib64" / name, ET_DYN, _build_id_note(host_id)
            )
            host_paths.append(str(host_lib))
            if sysroot_id is not None:
                _write_elf(
                    self.sysroot / str(host_lib).lstrip("/"), ET_DYN, _build_id_note(sysroot_id)
                )
        core = _write_elf(
            self.root / "dump_mongod.1.core",
            gdb_rbe_sysroot.ET_CORE,
            _nt_file_note([str(self.root / "bin" / "mongod"), *host_paths]),
        )
        with mock.patch.object(gdb_rbe_sysroot, "core_build_ids", return_value=loaded):
            return gdb_rbe_sysroot.core_uses_rbe_sysroot(str(core), str(self.sysroot), "eu-unstrip")

    def test_core_loaded_sysroot_libraries(self) -> None:
        self.assertTrue(self._uses_sysroot({"libc.so.6": ("aaaa", "bbbb")}, {"bbbb"}))

    def test_core_loaded_host_libraries(self) -> None:
        self.assertFalse(self._uses_sysroot({"libc.so.6": ("aaaa", "bbbb")}, {"aaaa"}))

    def test_identical_libraries(self) -> None:
        self.assertFalse(self._uses_sysroot({"libc.so.6": ("aaaa", "aaaa")}, {"aaaa"}))

    def test_library_missing_from_sysroot(self) -> None:
        self.assertFalse(self._uses_sysroot({"libc.so.6": ("aaaa", None)}, {"bbbb"}))

    def test_host_core_with_shared_and_drifted_libraries(self) -> None:
        # A host core where libc is identical in both, libssl matches only the sysroot because the
        # spawn host's image drifted from the task host's, and two libraries match only the host.
        libs = {
            "libc.so.6": ("c0", "c0"),
            "libssl.so.3": ("a1", "a0"),
            "libkrb5.so.3": ("b0", "b1"),
            "libcurl.so.4": ("d0", "d1"),
        }
        self.assertFalse(self._uses_sysroot(libs, {"c0", "a0", "b0", "d0"}))

    def test_remote_core_with_shared_and_drifted_libraries(self) -> None:
        libs = {
            "libc.so.6": ("c0", "c0"),
            "libssl.so.3": ("a0", "a1"),
            "libkrb5.so.3": ("b0", "b1"),
            "libcurl.so.4": ("d0", "d1"),
        }
        self.assertTrue(self._uses_sysroot(libs, {"c0", "a0", "b1", "d1"}))

    def test_tie_keeps_host(self) -> None:
        libs = {"libssl.so.3": ("a0", "a1"), "libkrb5.so.3": ("b0", "b1")}
        self.assertFalse(self._uses_sysroot(libs, {"a0", "b1"}))


@linux_only
class BuildGdbCommandTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        self.sysroot = self.root / "rbe_sysroot"
        self.sysroot.mkdir()
        self.core = str(_write_elf(self.root / "dump.core", gdb_rbe_sysroot.ET_CORE, b""))
        self.binary = str(_write_elf(self.root / "mongod", ET_DYN, b""))
        patches = [
            mock.patch.object(gdb_rbe_sysroot, "_find_eu_unstrip", return_value="eu-unstrip"),
            mock.patch.object(gdb_rbe_sysroot, "core_uses_rbe_sysroot", return_value=True),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _command(self, gdb_args: list[str], sysroot: pathlib.Path | None = None) -> list[str]:
        return gdb_rbe_sysroot.build_gdb_command("gdb", str(sysroot or self.sysroot), gdb_args)

    def test_adds_sysroot_for_positional_core(self) -> None:
        self.assertEqual(
            self._command([self.binary, self.core]),
            ["gdb", "-iex", f"set sysroot {self.sysroot}", self.binary, self.core],
        )

    def test_adds_sysroot_for_core_option(self) -> None:
        for args in ([self.binary, f"--core={self.core}"], [self.binary, "-c", self.core]):
            with self.subTest(args=args):
                self.assertEqual(self._command(args)[1:3], ["-iex", f"set sysroot {self.sysroot}"])

    def test_host_core_unchanged(self) -> None:
        gdb_rbe_sysroot.core_uses_rbe_sysroot.return_value = False
        self.assertEqual(self._command([self.binary, self.core]), ["gdb", self.binary, self.core])

    def test_no_core_unchanged(self) -> None:
        self.assertEqual(self._command([self.binary]), ["gdb", self.binary])
        gdb_rbe_sysroot.core_uses_rbe_sysroot.assert_not_called()

    def test_user_sysroot_respected(self) -> None:
        args = ["-ex", "set sysroot /elsewhere", self.binary, self.core]
        self.assertEqual(self._command(args), ["gdb", *args])

    def test_binary_inside_sysroot_still_gets_sysroot(self) -> None:
        binary = str(self.sysroot / "usr" / "bin" / "coreutils")
        self.assertEqual(
            self._command([binary, self.core])[1:3], ["-iex", f"set sysroot {self.sysroot}"]
        )

    def test_missing_sysroot_unchanged(self) -> None:
        self.assertEqual(
            self._command([self.binary, self.core], sysroot=self.root / "missing"),
            ["gdb", self.binary, self.core],
        )

    def test_inspection_failure_unchanged(self) -> None:
        gdb_rbe_sysroot.core_uses_rbe_sysroot.side_effect = OSError("unreadable")
        self.assertEqual(self._command([self.binary, self.core]), ["gdb", self.binary, self.core])

    def test_core_after_args_is_an_inferior_argument(self) -> None:
        for flag in ("--args", "-args"):
            with self.subTest(flag=flag):
                args = [flag, self.binary, self.core]
                self.assertEqual(self._command(args), ["gdb", *args])
        gdb_rbe_sysroot.core_uses_rbe_sysroot.assert_not_called()


if __name__ == "__main__":
    unittest.main()
