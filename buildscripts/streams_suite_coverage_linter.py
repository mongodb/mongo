#!/usr/bin/env python3
"""Verify every jstest under streams directories is referenced by at least one resmoke suite.

Exits 0 when all test files are covered exactly once, 1 otherwise.

Usage:
    python3 buildscripts/streams_suite_coverage_linter.py
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

# Suites that intentionally re-include tests from other suites (e.g. smoke
# suites with broad globs).  These are still checked for coverage but are
# excluded from the duplicate-detection logic.
DUPLICATE_EXEMPT_SUITES: set[str] = {
    "streams_smoke.yml",
}

# Filename patterns for non-test helper files that are loaded by tests
# rather than executed directly by resmoke.
NON_TEST_PATTERNS: list[str] = [
    "utils.js",
    "*_utils.js",
    "*_helper.js",
    "*_common.js",
    "*_harness.js",
    "common_test.js",
    "benchmark_utils.js",
    "fake_client.js",
]

# Directories whose contents are never standalone tests (libraries,
# fixture data, helper functions, etc.).
NON_TEST_DIRS: set[str] = {
    "lib",
    "libs",
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


def get_suite_covered_files() -> tuple[set[str], dict[str, list[str]]]:
    """Parse all streams suite YAMLs and return covered files and duplicates.

    Returns a tuple of (all covered paths, dict of path -> list of suite names for duplicates).
    All paths are relative to REPO_ROOT.
    """
    covered: set[str] = set()
    test_to_suites: dict[str, list[str]] = {}

    suite_files = sorted(SUITE_DIR.glob(SUITE_GLOB))
    for suite_file in suite_files:
        with open(suite_file, encoding="utf-8") as fh:
            config = yaml.safe_load(fh)

        if not config or "selector" not in config:
            continue

        selector = config["selector"]
        matched: set[str] = set()

        for root_pattern in selector.get("roots", []):
            full_pattern = str(REPO_ROOT / _normalize_suite_path(root_pattern))
            for match in glob.glob(full_pattern):
                matched.add(os.path.relpath(match, REPO_ROOT))

        for excl_pattern in selector.get("exclude_files", []):
            full_pattern = str(REPO_ROOT / _normalize_suite_path(excl_pattern))
            for match in glob.glob(full_pattern):
                matched.add(os.path.relpath(match, REPO_ROOT))

        for path in matched:
            covered.add(path)
            if suite_file.name not in DUPLICATE_EXEMPT_SUITES:
                test_to_suites.setdefault(path, []).append(suite_file.name)

    duplicates = {t: suites for t, suites in test_to_suites.items() if len(suites) > 1}
    return covered, duplicates


def main() -> int:
    all_tests = find_all_js_test_files()
    covered, duplicates = get_suite_covered_files()
    uncovered = sorted(all_tests - covered)
    has_errors = False

    if duplicates:
        has_errors = True
        print("ERROR: The following tests are configured in multiple suites:\n")
        for test in sorted(duplicates):
            print(f"  {test}")
            print(f"    Appears in: {', '.join(sorted(duplicates[test]))}")
        print("\nEach test should appear in exactly ONE suite file.\n")

    if uncovered:
        has_errors = True
        print(
            f"ERROR: Found {len(uncovered)} streams jstest(s) not referenced"
            " by any resmoke suite:\n"
        )
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

    if has_errors:
        return 1

    print("All streams jstests are covered by exactly one resmoke suite!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
