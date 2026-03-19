#!/usr/bin/env python3
"""Materialize clang-tidy IDE integration files from Bazel outputs."""

import argparse
import os
import pathlib
import platform
import sys

PLUGIN_CANDIDATES = [
    "libmongo_tidy_checks.so",
    "libmongo_tidy_checks.dylib",
    "mongo_tidy_checks.dll",
    "libmongo_tidy_checks.dll",
]


def mongo_tidy_checks_supported_platform() -> bool:
    if platform.system() != "Linux":
        return False


def clang_tidy_setup_recovery_message() -> str:
    if mongo_tidy_checks_supported_platform():
        return "Run `bazel run //:setup_clang_tidy` to materialize the clang-tidy config and mongo_tidy_checks plugin."

    return (
        "clang-tidy setup via Bazel is not supported on this platform. "
        "The mongo_tidy_checks plugin is only supported on Linux excluding Ubuntu 18.04."
    )


def _copy_if_changed(src: pathlib.Path, dst: pathlib.Path) -> bool:
    src_bytes = src.read_bytes()
    if dst.exists() and dst.read_bytes() == src_bytes:
        return False
    dst.write_bytes(src_bytes)
    return True


def _write_if_changed(path: pathlib.Path, contents: str) -> bool:
    if path.exists() and path.read_text(encoding="utf-8") == contents:
        return False
    path.write_text(contents, encoding="utf-8")
    return True


def materialize_clang_tidy_ide_files(
    repo_root: pathlib.Path,
    config_src: pathlib.Path,
    plugin_src: pathlib.Path,
) -> tuple[bool, bool]:
    config_changed = _copy_if_changed(config_src, repo_root / ".clang-tidy")
    marker_changed = _write_if_changed(
        repo_root / ".mongo_checks_module_path", str(plugin_src.resolve())
    )
    return config_changed, marker_changed


def _resolve_runfile(r, rlocation_path: str) -> pathlib.Path:
    resolved = r.Rlocation(rlocation_path)
    if not resolved:
        raise FileNotFoundError(f"Failed to resolve Bazel runfile: {rlocation_path}")
    path = pathlib.Path(resolved)
    if not path.is_file():
        raise FileNotFoundError(f"Resolved runfile is not a file: {path}")
    return path.resolve()


def _resolve_plugin(
    r, workspace_prefix: str, package_path: str = "src/mongo/tools/mongo_tidy_checks"
) -> pathlib.Path:
    for candidate in PLUGIN_CANDIDATES:
        resolved = r.Rlocation(f"{workspace_prefix}/{package_path}/{candidate}")
        if resolved and pathlib.Path(resolved).is_file():
            return pathlib.Path(resolved).resolve()

    candidate_list = ", ".join(PLUGIN_CANDIDATES)
    raise FileNotFoundError(
        "Failed to resolve mongo_tidy_checks plugin from Bazel runfiles. "
        f"Tried: {candidate_list}"
    )


def main() -> int:
    try:
        import runfiles
    except ModuleNotFoundError:
        print(
            "The `bazel-runfiles` dependency is required to run `bazel run //:setup_clang_tidy`.",
            file=sys.stderr,
        )
        return 1

    parser = argparse.ArgumentParser()
    parser.add_argument("--config-rlocation", required=True)
    args = parser.parse_args()

    workspace_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not workspace_dir:
        print("This tool must be run with `bazel run //:setup_clang_tidy`.", file=sys.stderr)
        return 1

    repo_root = pathlib.Path(workspace_dir).resolve()
    config_rlocation = args.config_rlocation
    if "/" not in config_rlocation:
        print(
            f"Unexpected config runfile path: {config_rlocation}",
            file=sys.stderr,
        )
        return 1

    workspace_prefix = config_rlocation.split("/", 1)[0]
    r = runfiles.Create()
    if r is None:
        print("Failed to initialize Bazel runfiles support.", file=sys.stderr)
        return 1

    config_src = _resolve_runfile(r, config_rlocation)
    plugin_src = _resolve_plugin(r, workspace_prefix)

    config_changed, marker_changed = materialize_clang_tidy_ide_files(
        repo_root,
        config_src,
        plugin_src,
    )

    print(f"Configured clang-tidy for IDE use at {repo_root / '.clang-tidy'}")
    print(f"Configured mongo tidy checks plugin at {plugin_src}")
    if not config_changed and not marker_changed:
        print("clang-tidy IDE files were already up to date.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
