#!/usr/bin/env python3
"""Fully disabled feature flag tags check.

Check that no jstest is newly tagged with a feature flag listed in
fully_disabled_feature_flags.yml.

A test tagged with such a feature flag can never run: the flag is forced off on the
"all feature flags" variants, and it defaults to false on the "no feature flags"
variant, so the test is excluded from every resmoke suite. The test is dead on
arrival: it gives the false impression of coverage while never actually executing.

Only tags introduced by the patch/commit under test are flagged. Tests that were
already tagged with a fully disabled feature flag before the patch are left alone,
including tests that the patch renamed.

Feature flags that some Evergreen task or variant enables via --additionalFeatureFlags
are exempt from this check: tests tagged with such a flag do run on that task or
variant.
"""

import argparse
import os
import re
import subprocess
import sys
from collections.abc import Collection

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import yaml

from buildscripts.resmokelib import selector
from buildscripts.resmokelib.utils import globstar, jscomment

FULLY_DISABLED_FEATURE_FLAGS_FILE = "buildscripts/resmokeconfig/fully_disabled_feature_flags.yml"
# The root Evergreen project configuration. Its include graph identifies every active config,
# including module configs outside etc/.
EVERGREEN_CONFIG_FILE = "etc/evergreen.yml"
# Matches "--additionalFeatureFlags=<flags>", "--additionalFeatureFlags <flags>" and the
# single-quoted or double-quoted form of the latter, where <flags> is a comma-separated
# list of feature flags.
_ADDITIONAL_FEATURE_FLAGS_PATTERN = re.compile(
    r"--additionalFeatureFlags(?:=|[ \t]+)[\"']?([A-Za-z0-9_, \t]+)"
)
# These fields carry arguments ultimately passed to test or shell commands. Parsing only their
# scalar values prevents prose fields such as descriptions from exempting a feature flag.
_EVERGREEN_ARGUMENT_FIELDS = frozenset(
    {"args", "post", "pre", "resmoke_args", "script", "test_flags"}
)
JSTEST_ROOTS = [
    "jstests/**/*.js",
    "src/mongo/db/modules/atlas/jstests/**/*.js",
]
ENTERPRISE_JSTEST_ROOT = "{enterprise_path}/jstests/**/*.js"


def _get_evergreen_command_arguments(config: object) -> list[str]:
    """Return argument strings from the parsed Evergreen configuration."""
    if isinstance(config, list):
        return [
            argument for value in config for argument in _get_evergreen_command_arguments(value)
        ]
    if not isinstance(config, dict):
        return []

    arguments = []
    for field, value in config.items():
        if field in _EVERGREEN_ARGUMENT_FIELDS:
            if isinstance(value, str):
                arguments.append(value)
            elif isinstance(value, list):
                arguments.extend(item for item in value if isinstance(item, str))
        else:
            arguments.extend(_get_evergreen_command_arguments(value))
    return arguments


def get_active_evergreen_config_files(config_path: str = EVERGREEN_CONFIG_FILE) -> list[str]:
    """Get the root Evergreen config and every config it includes."""
    config_files = []
    pending_files = [config_path]
    while pending_files:
        pathname = pending_files.pop()
        if pathname in config_files:
            continue
        config_files.append(pathname)
        with open(pathname, encoding="utf8", errors="replace") as fh:
            config = yaml.safe_load(fh)
        if not isinstance(config, dict):
            continue
        for include in config.get("include", []):
            if isinstance(include, dict) and isinstance(filename := include.get("filename"), str):
                pending_files.append(filename)
    return config_files


def _run_git_cmd(cmd_args: list[str]) -> None:
    """Run a git command, raising with git's own output on failure."""
    proc = subprocess.run(["git"] + cmd_args, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"git {' '.join(cmd_args)} failed:\n{proc.stderr}")


def discover_jstest_tags(enterprise_path: str | None = None) -> dict[str, list[str]]:
    """Discover all jstests and return a mapping of test path to the tags of that test."""
    candidate_roots = list(JSTEST_ROOTS)
    if enterprise_path:
        candidate_roots.append(ENTERPRISE_JSTEST_ROOT.format(enterprise_path=enterprise_path))
    # A root that matches no file makes selector.filter_tests raise, because it compares the
    # unmatched path against a jstests directory that is only set when running under resmoke.
    # Drop those roots so that a module missing from this checkout narrows the check instead
    # of failing it outright, which on the merge queue would block every commit.
    roots = [root for root in candidate_roots if globstar.glob(root)]
    selector_config = {
        "roots": roots,
    }
    tests, _ = selector.filter_tests("js_test", selector_config)

    # The file list and the tags are cached; clear both so that discovery reflects the current
    # state of the working tree, which this script changes between the two passes.
    globstar.glob.cache_clear()
    jscomment.get_tags.cache_clear()
    test_tags = {}
    for test in tests:
        try:
            test_tags[test] = jscomment.get_tags(test)
        except FileNotFoundError:
            continue
    return test_tags


def load_fully_disabled_flags(pathname: str) -> list[str]:
    """Load the list of fully disabled feature flags."""
    with open(pathname, encoding="utf8") as fh:
        fully_disabled_flags = yaml.safe_load(fh)
    return fully_disabled_flags or []


def get_evergreen_enabled_feature_flags(yml_files: Collection[str] | None = None) -> set[str]:
    """Get the feature flags some Evergreen task or variant enables via --additionalFeatureFlags.

    A fully disabled feature flag is not dead everywhere if a task or variant turns it on with
    --additionalFeatureFlags, so tests tagged with it do run there. Newly tagging a test with
    such a flag must not fail this check.
    """
    enabled_flags = set()
    for pathname in get_active_evergreen_config_files() if yml_files is None else yml_files:
        if not pathname.endswith((".yml", ".yaml")) or not os.path.isfile(pathname):
            continue
        with open(pathname, encoding="utf8", errors="replace") as fh:
            for argument in _get_evergreen_command_arguments(yaml.safe_load(fh)):
                for match in _ADDITIONAL_FEATURE_FLAGS_PATTERN.finditer(argument):
                    enabled_flags.update(
                        flag.strip() for flag in match.group(1).split(",") if flag.strip()
                    )
    return enabled_flags


def get_renamed_tests(diff_file_name: str) -> dict[str, str]:
    """Get the {old path: new path} renames performed by the diff.

    The diff must be generated with rename detection (git diff --find-renames) for renames to
    be recognized; otherwise a renamed test is indistinguishable from a deleted test plus a
    new test. Copies are intentionally not reported as renames.
    """
    renames = {}
    rename_from = None
    with open(diff_file_name, encoding="utf8", errors="replace") as fh:
        for line in fh:
            if line.startswith("diff --git "):
                rename_from = None
            elif line.startswith("rename from "):
                rename_from = line[len("rename from ") :].rstrip("\n")
            elif line.startswith("rename to ") and rename_from is not None:
                renames[rename_from] = line[len("rename to ") :].rstrip("\n")
                rename_from = None
    return renames


def get_new_fully_disabled_tagged_tests(
    base_tags: dict[str, list[str]],
    patch_tags: dict[str, list[str]],
    fully_disabled_flags: Collection[str],
    renames: dict[str, str] | None = None,
) -> list[str]:
    """Get the list of tests newly tagged with a fully disabled feature flag.

    A test is flagged when the patched version of the test is tagged with a fully disabled
    feature flag that the base version of the test was not tagged with. A test renamed by the
    diff is also compared against its old path, so renaming a test does not make its
    pre-existing tags look newly introduced.
    """
    disabled_flags = set(fully_disabled_flags)
    renamed_from = {new_path: old_path for old_path, new_path in (renames or {}).items()}
    found_tests = []
    for test in sorted(patch_tags):
        # Consider both the test's own path and, when the diff renamed it, the path it came
        # from: either one having carried the tag before the patch means it is not new.
        previous_paths = [test]
        if test in renamed_from:
            previous_paths.append(renamed_from[test])
        previous_tags = {tag for path in previous_paths for tag in base_tags.get(path, [])}
        new_disabled_tags = [
            tag for tag in patch_tags[test] if tag in disabled_flags and tag not in previous_tags
        ]
        if new_disabled_tags:
            found_tests.append(f"{test}: {', '.join(new_disabled_tags)}")
    return found_tests


def main(diff_file_name: str, enterprise_path: str | None = None) -> None:
    """Run the main function."""
    if diff_file_name and os.path.getsize(diff_file_name) == 0:
        # An empty diff means the patch introduces no changes, so there is nothing to check.
        sys.exit(0)

    fully_disabled_flags = [
        flag
        for flag in load_fully_disabled_flags(FULLY_DISABLED_FEATURE_FLAGS_FILE)
        if flag not in get_evergreen_enabled_feature_flags()
    ]
    renames = get_renamed_tests(diff_file_name)
    patch_tags = discover_jstest_tags(enterprise_path)

    _run_git_cmd(["apply", "-R", diff_file_name])
    try:
        base_tags = discover_jstest_tags(enterprise_path)
    finally:
        _run_git_cmd(["apply", diff_file_name])

    new_fully_disabled_tagged_tests = get_new_fully_disabled_tagged_tests(
        base_tags, patch_tags, fully_disabled_flags, renames
    )

    if new_fully_disabled_tagged_tests:
        print(
            "Found jstests newly tagged with a feature flag from "
            f"{FULLY_DISABLED_FEATURE_FLAGS_FILE}. Such a flag is forced off on the 'all feature "
            "flags' variants and defaults to false elsewhere, so these tests are excluded from "
            "every suite and will never run:\n"
            + "\n".join(new_fully_disabled_tagged_tests)
            + "\n\nRemove the tag from these tests."
        )
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--diff-file-name", type=str, required=True, help="Name of the file containing the git diff"
    )
    parser.add_argument("--enterprise-path", type=str, help="Path to the enterprise module")
    args = parser.parse_args()
    main(args.diff_file_name, args.enterprise_path)
