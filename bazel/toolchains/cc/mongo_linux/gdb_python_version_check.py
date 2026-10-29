"""Checks that the GDB wrapper follows Bazel's hermetic Python version."""

import pathlib
import re
import sys


def check_gdb_wrapper_python_version(
    wrapper_path: pathlib.Path, expected_version: str | None = None
) -> list[str]:
    """Return errors when the GDB wrapper's Python version is inconsistent."""

    if expected_version is None:
        expected_version = f"{sys.version_info.major}.{sys.version_info.minor}"

    try:
        wrapper = wrapper_path.read_text(encoding="utf-8")
    except OSError as error:
        return [f"Could not read {wrapper_path}: {error}"]

    errors = []
    configured_version_match = re.search(
        r'^\s*python3_version\s*=\s*"([0-9]+\.[0-9]+)"', wrapper, re.MULTILINE
    )
    if configured_version_match is None:
        errors.append(f"{wrapper_path} does not define python3_version")
    elif configured_version_match.group(1) != expected_version:
        errors.append(
            f"{wrapper_path} uses Python {configured_version_match.group(1)}, "
            f"but Bazel's hermetic Python is {expected_version}"
        )

    expected_stow_component = f"python{expected_version.replace('.', '')}-"
    if f"stow/{expected_stow_component}" not in wrapper:
        errors.append(
            f"{wrapper_path} does not reference the Python {expected_version} "
            f"stow component ({expected_stow_component})"
        )

    return errors
