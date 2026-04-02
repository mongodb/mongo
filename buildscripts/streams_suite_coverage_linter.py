#!/usr/bin/env python3
"""Verify every jstest under streams directories is referenced by at least one resmoke suite.

Exits 0 when all test files are covered, 1 otherwise.

Usage:
    python3 buildscripts/resmoke_suite_coverage_linter.py
"""

import fnmatch
import glob
import os
import pathlib
import sys

import yaml

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

ENTERPRISE_MODULE = REPO_ROOT / "src" / "mongo" / "db" / "modules" / "enterprise"

JSTESTS_DIRS = [
    ENTERPRISE_MODULE / "jstests" / "streams",
    ENTERPRISE_MODULE / "jstests" / "streams_kafka",
]

SUITE_DIR = REPO_ROOT / "buildscripts" / "resmokeconfig" / "suites"

SUITE_GLOB = "streams*.yml"

# Filename patterns for non-test helper files that are loaded by tests
# rather than executed directly by resmoke.
NON_TEST_PATTERNS: list[str] = [
    "*_utils.js",
    "benchmark_utils.js",
    "*_harness.js",
    "*_common.js",
]

# Directories whose contents are never standalone tests (libraries,
# fixture data, helper functions, etc.).
NON_TEST_DIRS: set[str] = {
    "lib",
    "data",
    "function",
    "mongostream_container_manager",
}


def _is_non_test_file(filename: str) -> bool:
    return any(fnmatch.fnmatch(filename, pat) for pat in NON_TEST_PATTERNS)


def _is_in_non_test_dir(path: pathlib.Path, jstests_dir: pathlib.Path) -> bool:
    rel = path.relative_to(jstests_dir)
    return any(part in NON_TEST_DIRS for part in rel.parts)


def find_all_js_test_files() -> set[str]:
    """Find all .js files under streams jstests directories, excluding known non-test files.

    Returns paths relative to REPO_ROOT.
    """
    files: set[str] = set()
    for jstests_dir in JSTESTS_DIRS:
        if not jstests_dir.is_dir():
            continue
        for root, _dirs, filenames in os.walk(jstests_dir):
            root_path = pathlib.Path(root)
            if _is_in_non_test_dir(root_path, jstests_dir):
                continue
            for f in filenames:
                if not f.endswith(".js"):
                    continue
                if _is_non_test_file(f):
                    continue
                full = root_path / f
                files.add(str(full.relative_to(REPO_ROOT)))
    return files


def _normalize_suite_path(path: str) -> str:
    """Replace the module wildcard with the concrete enterprise path."""
    return path.replace("modules/*/", "modules/enterprise/")


def get_suite_covered_files() -> set[str]:
    """Parse all streams suite YAMLs and return the set of covered file paths.

    Returns paths relative to REPO_ROOT.
    """
    covered: set[str] = set()

    suite_files = sorted(SUITE_DIR.glob(SUITE_GLOB))
    for suite_file in suite_files:
        with open(suite_file, encoding="utf-8") as fh:
            config = yaml.safe_load(fh)

        if not config or "selector" not in config:
            continue

        selector = config["selector"]

        for root_pattern in selector.get("roots", []):
            full_pattern = str(REPO_ROOT / _normalize_suite_path(root_pattern))
            for match in glob.glob(full_pattern):
                covered.add(os.path.relpath(match, REPO_ROOT))

        for excl_pattern in selector.get("exclude_files", []):
            full_pattern = str(REPO_ROOT / _normalize_suite_path(excl_pattern))
            for match in glob.glob(full_pattern):
                covered.add(os.path.relpath(match, REPO_ROOT))

    return covered


def main() -> int:
    all_tests = find_all_js_test_files()
    covered = get_suite_covered_files()
    uncovered = sorted(all_tests - covered)

    if not uncovered:
        print("All streams jstests are covered by resmoke suites!")
        return 0

    print(f"Found {len(uncovered)} streams jstest(s) not referenced by any resmoke suite:\n")
    for f in uncovered:
        print(f"  {f}")
    print(
        "\nPlease add each test to the appropriate streams*.yml suite config in\n"
        "buildscripts/resmokeconfig/suites/\n"
        "\n"
        "If a file is not a standalone test (e.g. a utility loaded by other tests),\n"
        "add its filename pattern to NON_TEST_PATTERNS or NON_TEST_DIRS in\n"
        "buildscripts/streams_suite_coverage_linter.py"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
