#!/usr/bin/env python3
"""Run gdb, pointing it at the RBE container sysroot for cores produced under remote execution.

Processes that ran inside the remote execution container loaded that container's system libraries,
so gdb can only unwind through them when `set sysroot` points at a copy of the container filesystem
(see create_rbe_sysroot.py). Cores from processes that ran on the host itself must keep using the
host libraries, so the choice is made per invocation by comparing the build-ids recorded in the core
against the host and sysroot copies of each library the process had mapped.

Usage: gdb_rbe_sysroot.py <gdb> <sysroot> [gdb arguments...]

This runs under the toolchain python on spawn hosts, so it must only use the standard library.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
from collections.abc import Iterator
from typing import BinaryIO

ET_CORE = 4
PT_NOTE = 4
NT_GNU_BUILD_ID = 3
NT_FILE = 0x46494C45


class _ElfHeader:
    def __init__(self, ident: bytes, f: BinaryIO) -> None:
        self.is64 = ident[4] == 2
        self.endian = "<" if ident[5] == 1 else ">"
        if self.is64:
            fields = struct.unpack(self.endian + "HHIQQQIHHHHHH", f.read(48))
        else:
            fields = struct.unpack(self.endian + "HHIIIIIHHHHHH", f.read(36))
        self.e_type = fields[0]
        self.e_phoff = fields[4]
        self.e_phentsize = fields[8]
        self.e_phnum = fields[9]


def _read_header(f: BinaryIO) -> _ElfHeader | None:
    ident = f.read(16)
    if len(ident) < 16 or ident[:4] != b"\x7fELF":
        return None
    return _ElfHeader(ident, f)


def _iter_notes(f: BinaryIO, header: _ElfHeader) -> Iterator[tuple[bytes, int, bytes]]:
    """Yields (name, type, desc) for every note in the file's PT_NOTE segments."""
    segments = []
    for i in range(header.e_phnum):
        f.seek(header.e_phoff + i * header.e_phentsize)
        if header.is64:
            p_type, _, p_offset, _, _, p_filesz, _, p_align = struct.unpack(
                header.endian + "IIQQQQQQ", f.read(56)
            )
        else:
            p_type, p_offset, _, _, p_filesz, _, _, p_align = struct.unpack(
                header.endian + "IIIIIIII", f.read(32)
            )
        if p_type == PT_NOTE:
            segments.append((p_offset, p_filesz, 8 if p_align == 8 else 4))

    for offset, size, align in segments:
        f.seek(offset)
        data = f.read(size)
        pos = 0
        while pos + 12 <= len(data):
            namesz, descsz, note_type = struct.unpack_from(header.endian + "III", data, pos)
            pos += 12
            name = data[pos : pos + namesz]
            pos += (namesz + align - 1) & ~(align - 1)
            desc = data[pos : pos + descsz]
            pos += (descsz + align - 1) & ~(align - 1)
            yield name, note_type, desc


def is_core_file(path: str) -> bool:
    try:
        with open(path, "rb") as f:
            header = _read_header(f)
    except (OSError, struct.error):
        return False
    return header is not None and header.e_type == ET_CORE


def file_build_id(path: str) -> str | None:
    try:
        with open(path, "rb") as f:
            header = _read_header(f)
            if header is None:
                return None
            for name, note_type, desc in _iter_notes(f, header):
                if name.rstrip(b"\0") == b"GNU" and note_type == NT_GNU_BUILD_ID:
                    return desc.hex()
    except (OSError, struct.error):
        pass
    return None


def core_mapped_files(core_path: str) -> list[str]:
    """Returns the files the crashed process had mapped, as recorded in the core's NT_FILE note."""
    with open(core_path, "rb") as f:
        header = _read_header(f)
        if header is None:
            return []
        word = "Q" if header.is64 else "I"
        word_size = struct.calcsize(word)
        for name, note_type, desc in _iter_notes(f, header):
            if name.rstrip(b"\0") != b"CORE" or note_type != NT_FILE:
                continue
            (count,) = struct.unpack_from(header.endian + word, desc, 0)
            names = desc[word_size * (2 + 3 * count) :].split(b"\0")[:count]
            return list(dict.fromkeys(os.fsdecode(n) for n in names))
    return []


def parse_unstrip_build_ids(output: str) -> set[str]:
    """Parses `eu-unstrip -n --core` output, whose lines look like `START+SIZE BUILDID@ADDR ...`."""
    build_ids = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and "@" in fields[1]:
            build_id = fields[1].split("@", 1)[0]
            if build_id != "-":
                build_ids.add(build_id)
    return build_ids


def core_build_ids(core_path: str, eu_unstrip: str) -> set[str]:
    """Returns the build-ids of the modules that were loaded in the crashed process."""
    result = subprocess.run(
        [eu_unstrip, "-n", f"--core={core_path}"],
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
    )
    return parse_unstrip_build_ids(result.stdout)


def core_uses_rbe_sysroot(core_path: str, sysroot: str, eu_unstrip: str) -> bool:
    """Returns whether more of the core's libraries match the sysroot than match the host.

    Amazon Linux hosts share many identical packages with the container, and a spawn host's image
    can drift from the one the task ran on, so a single library matching only the sysroot is not
    enough to tell where the process ran.
    """
    loaded = core_build_ids(core_path, eu_unstrip)
    sysroot_matches = 0
    host_matches = 0
    for path in core_mapped_files(core_path):
        if not os.path.isabs(path):
            continue
        sysroot_id = file_build_id(os.path.join(sysroot, path.lstrip("/")))
        host_id = file_build_id(path)
        if sysroot_id == host_id:
            continue
        if sysroot_id in loaded:
            sysroot_matches += 1
        elif host_id in loaded:
            host_matches += 1
    return sysroot_matches > host_matches


def _core_args(gdb_args: list[str]) -> list[str]:
    candidates = []
    for arg in gdb_args:
        if arg in ("--args", "-args"):
            # Everything after --args is the program to run and its arguments.
            break
        if arg.startswith("-") and "=" in arg:
            arg = arg.split("=", 1)[1]
        if os.path.isfile(arg) and is_core_file(arg):
            candidates.append(arg)
    return candidates


def _find_eu_unstrip(gdb: str) -> str | None:
    sibling = os.path.join(os.path.dirname(gdb), "eu-unstrip")
    if os.access(sibling, os.X_OK):
        return sibling
    return shutil.which("eu-unstrip")


def build_gdb_command(gdb: str, sysroot: str, gdb_args: list[str]) -> list[str]:
    command = [gdb, *gdb_args]
    if not os.path.isdir(sysroot) or any(
        "set sysroot" in arg or "set solib-absolute-prefix" in arg for arg in gdb_args
    ):
        return command

    eu_unstrip = _find_eu_unstrip(gdb)
    if eu_unstrip is None:
        return command

    for core in _core_args(gdb_args):
        try:
            uses_sysroot = core_uses_rbe_sysroot(core, sysroot, eu_unstrip)
        except (OSError, subprocess.SubprocessError, struct.error) as err:
            print(
                f"Could not inspect {core} for remote execution libraries: {err}", file=sys.stderr
            )
            continue
        if uses_sysroot:
            print(
                f"{core} was produced inside the remote execution container, "
                f"loading system libraries from {sysroot}",
                file=sys.stderr,
            )
            return [gdb, "-iex", f"set sysroot {sysroot}", *gdb_args]
    return command


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    gdb, sysroot, *gdb_args = sys.argv[1:]
    command = build_gdb_command(gdb, sysroot, gdb_args)
    os.execv(gdb, command)


if __name__ == "__main__":
    sys.exit(main())
