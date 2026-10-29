"""Creates case variants of Windows import libraries for Linux link actions."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

_LIB_DIRS = (
    "msvc/lib/x64",
    "msvc/atlmfc/lib/x64",
    "winsdk/lib/ucrt/x64",
    "winsdk/lib/um/x64",
)


def _copy_alias(source: Path, destination: Path, alias_name: str) -> None:
    if alias_name == source.name:
        return

    destination.mkdir(parents=True, exist_ok=True)
    alias = destination / alias_name
    if not alias.exists():
        shutil.copy2(source, alias)


def create_lib_aliases(repo: Path) -> None:
    """Copies SDK/MSVC libraries under common capitalization variants for Linux linking.

    Link flags and object-file directives can request MSVCRTD.lib while the sysroot
    contains msvcrtd.lib. Windows normally resolves both names to the same file, but
    Linux filenames are case-sensitive. These copies preserve the library contents;
    the toolchain adds their directories to /LIBPATH and declares them as remote inputs.

    Each capitalization variant lives in a separate directory so the copies can also
    be created on case-insensitive Windows filesystems. This covers common spellings,
    rather than providing fully case-insensitive lookup, and leaves the sysroot intact.
    """
    shutil.rmtree(repo / "lib_aliases", ignore_errors=True)

    for relative_dir in _LIB_DIRS:
        source_dir = repo / "sysroot" / relative_dir
        if not source_dir.is_dir():
            continue

        for source in sorted(source_dir.iterdir(), key=lambda path: path.name):
            if not source.is_file() or source.suffix.lower() != ".lib":
                continue

            name = source.name
            lower_name = name.lower()
            first_upper_lower_rest_name = lower_name[:1].upper() + lower_name[1:]
            upper_base_lower_extension_name = source.stem.upper() + ".lib"
            original_base_lower_extension_name = source.stem + ".lib"

            alias_root = repo / "lib_aliases"
            _copy_alias(source, alias_root / "lower" / relative_dir, lower_name)
            _copy_alias(
                source,
                alias_root / "first_upper_lower_rest" / relative_dir,
                first_upper_lower_rest_name,
            )
            _copy_alias(
                source,
                alias_root / "upper_base_lower_extension" / relative_dir,
                upper_base_lower_extension_name,
            )
            _copy_alias(
                source,
                alias_root / "original_base_lower_extension" / relative_dir,
                original_base_lower_extension_name,
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    args = parser.parse_args()
    create_lib_aliases(args.repo)


if __name__ == "__main__":
    main()
