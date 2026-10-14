"""Keep the editor's version pin in sync with Bazel's clang-format."""

import argparse
import os
import subprocess
import tempfile
from pathlib import Path


def check_version(binary: Path, version_file: Path, *, fix: bool) -> bool:
    result = subprocess.run([str(binary), "--version"], capture_output=True, text=True, check=True)
    version = result.stdout.strip()
    if not version or "clang-format version " not in version or "\n" in version:
        raise ValueError(f"Unexpected clang-format version: {version!r}")
    expected = version_file.read_text().strip() if version_file.exists() else ""
    if expected == version:
        return True
    if fix:
        version_file.write_text(f"{version}\n")
        print(f"Updated {version_file} to {version}")
        return True
    print(f"{version_file} is out of date: expected {expected!r}, Bazel uses {version!r}.")
    print("Run bazel run lint --fix to update the editor formatter version pin.")
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang-format", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    binary = args.clang_format.resolve()
    root = Path(os.environ.get("BUILD_WORKSPACE_DIRECTORY", Path(__file__).resolve().parents[1]))
    if not check_version(
        binary, root / "buildscripts/clang_format_version.txt", fix=not args.check
    ):
        return 1
    # Prepare the editor cache even when Bazel convenience symlinks are disabled.
    cache = root / ".cache/clang-format-vscode"
    cache.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=cache.parent, delete=False) as temporary:
        temporary.write(f"{binary}\n")
    Path(temporary.name).replace(cache)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
