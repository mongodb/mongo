#!/usr/bin/env python3
"""Generate a Clang VFS overlay for case-insensitive Windows headers."""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path
from typing import Iterable

_INCLUDE_PATTERN = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)


def _collect_files(repo: Path, include_dirs: Iterable[str]) -> list[Path]:
    files: set[Path] = set()
    for include_dir in include_dirs:
        root = repo / "sysroot" / include_dir
        if root.is_dir():
            files.update(path for path in root.rglob("*") if path.is_file())
    return sorted(files, key=lambda path: path.as_posix())


def _add_alias(
    directories: dict[str, dict[str, str]],
    actual_file: Path,
    alias_name: str,
    repo: Path,
    execroot_prefix: str,
) -> None:
    if alias_name == actual_file.name:
        return

    relative_dir = actual_file.parent.relative_to(repo).as_posix()
    relative_file = actual_file.relative_to(repo).as_posix()
    directory_name = f"{execroot_prefix}/{relative_dir}"
    aliases = directories.setdefault(directory_name, {})
    aliases.setdefault(alias_name, f"{execroot_prefix}/{relative_file}")


def _add_standard_aliases(
    directories: dict[str, dict[str, str]],
    actual_file: Path,
    repo: Path,
    execroot_prefix: str,
) -> None:
    name = actual_file.name
    lower_name = name.lower()
    aliases = (
        lower_name,
        name[:1].upper() + name[1:],
        name[:1].upper() + lower_name[1:],
    )
    if lower_name == "winsock2.h":
        aliases += ("WinSock2.h",)

    for alias_name in aliases:
        _add_alias(directories, actual_file, alias_name, repo, execroot_prefix)


def _include_basename(include_name: bytes) -> str:
    # Includes may use either slash even when the repository is generated on
    # Windows. The VFS overlay only needs the basename because the actual
    # file's parent directory is where the alias is installed.
    return include_name.decode("utf-8", errors="ignore").replace("\\", "/").rsplit("/", 1)[-1]


def generate_overlay(
    repo: Path, execroot_prefix: str, include_dirs: Iterable[str]
) -> dict[str, object]:
    files = _collect_files(repo, include_dirs)
    files_by_lower: dict[str, list[Path]] = defaultdict(list)
    directories: dict[str, dict[str, str]] = {}

    for actual_file in files:
        files_by_lower[actual_file.name.lower()].append(actual_file)
        _add_standard_aliases(directories, actual_file, repo, execroot_prefix)

    for file in files:
        for match in _INCLUDE_PATTERN.finditer(file.read_bytes()):
            include_name = _include_basename(match.group(1))
            for actual_file in files_by_lower[include_name.lower()]:
                if actual_file.name != include_name:
                    _add_alias(directories, actual_file, include_name, repo, execroot_prefix)

    roots = [
        {
            "type": "directory",
            "name": directory_name,
            "contents": [
                {
                    "type": "file",
                    "name": alias_name,
                    "external-contents": external_contents,
                }
                for alias_name, external_contents in sorted(aliases.items())
            ],
        }
        for directory_name, aliases in sorted(directories.items())
    ]
    return {"version": 0, "roots": roots}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--execroot-prefix", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("include_dirs", nargs="+")
    args = parser.parse_args()

    overlay = generate_overlay(args.repo, args.execroot_prefix, args.include_dirs)
    args.output.write_text(json.dumps(overlay, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
