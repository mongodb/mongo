"""Directory structure checks for MongoDB distribution archives."""

import tarfile
from pathlib import Path, PurePosixPath


def validate_top_level_directory(archive_path: str | Path, target_arch: str) -> None:
    """Require one Linux target directory containing the packaged server binaries."""
    archive_arch = {
        "x86_64": "x86_64",
        "amd64": "x86_64",
        "arm64": "aarch64",
        "aarch64": "aarch64",
        "ppc64le": "ppc64le",
        "ppc64el": "ppc64le",
        "s390x": "s390x",
    }.get(target_arch)
    if archive_arch is None:
        raise ValueError(
            f"Unsupported target architecture for package directory checks: {target_arch}"
        )

    expected_prefix = f"mongodb-linux-{archive_arch}-"
    top_level_directories: set[str] = set()
    files: set[PurePosixPath] = set()
    with tarfile.open(archive_path, "r|*") as archive:
        for member in archive:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"Unexpected archive member path in {archive_path}: {member.name}")
            if not path.parts and member.isdir():
                continue
            if not path.parts:
                raise ValueError(f"Unexpected archive member path in {archive_path}: {member.name}")
            top_level_directories.add(path.parts[0])
            if member.isfile():
                files.add(path)

    if len(top_level_directories) != 1:
        raise ValueError(
            f"Expected exactly one top-level directory in {archive_path}; "
            f"found {sorted(top_level_directories)}"
        )
    directory = top_level_directories.pop()
    if not directory.startswith(expected_prefix) or directory == expected_prefix:
        raise ValueError(
            f"Expected package directory starting with '{expected_prefix}' "
            f"for target architecture '{target_arch}' in {archive_path}; found '{directory}'"
        )

    missing_files = [
        f"bin/{binary}"
        for binary in ("mongod", "mongos")
        if PurePosixPath(directory, "bin", binary) not in files
    ]
    if missing_files:
        raise ValueError(
            f"Missing required package files under '{directory}' in {archive_path}: "
            f"{', '.join(missing_files)}"
        )
