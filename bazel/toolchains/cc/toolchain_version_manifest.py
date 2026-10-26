"""Combine tool probes into build provenance and enforce production pins."""

import argparse
import json
from pathlib import Path

from buildscripts.package_test.toolchain_versions import validate_toolchain_versions


def make_manifest(
    compiler_file: Path,
    linker_file: Path,
    target_os: str,
    target_arch: str,
    toolchain_mode: str,
    production_release: bool,
) -> dict:
    manifest = {
        "target_os": target_os,
        "target_arch": target_arch,
        "toolchain_mode": toolchain_mode,
        "production_release": production_release,
        **json.loads(compiler_file.read_text()),
        **json.loads(linker_file.read_text()),
    }
    if production_release:
        validate_toolchain_versions(manifest)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler-version-file", type=Path, required=True)
    parser.add_argument("--linker-version-file", type=Path, required=True)
    parser.add_argument("--target-os", required=True)
    parser.add_argument("--target-arch", required=True)
    parser.add_argument("--toolchain-mode", choices=("native", "cross"), required=True)
    parser.add_argument("--production-release", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = make_manifest(
        args.compiler_version_file,
        args.linker_version_file,
        args.target_os,
        args.target_arch,
        args.toolchain_mode,
        args.production_release,
    )
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
