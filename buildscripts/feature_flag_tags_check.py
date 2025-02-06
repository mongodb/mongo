#!/usr/bin/env python3
"""Feature flag tags check.

Check that on changing feature flag from disabled to enabled by default in all js tests that
had that feature flag in tags there is a tag that requires the latest FCV.
"""

import argparse
import os
import subprocess
import sys

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.resmokelib import selector
from buildscripts.resmokelib.multiversionconstants import REQUIRES_FCV_TAG_LATEST
from buildscripts.resmokelib.utils import jscomment


def _run_git_cmd(cmd_args, cwd=None, silent=True):
    """Run git command."""
    run_args = {}
    if cwd:
        run_args["cwd"] = cwd
    if silent:
        run_args["stdout"] = subprocess.DEVNULL
        run_args["stderr"] = subprocess.DEVNULL
    subprocess.run(["git"] + cmd_args, **run_args, check=False)


def get_tests_with_feature_flag_tags(feature_flags, ent_path):
    """Get the list of tests with feature flag tag."""
    selector_config = {
        "roots": ["jstests/**/*.js", f"{ent_path}/jstests/**/*.js"],
        "include_with_any_tags": feature_flags,
    }
    tests, _ = selector.filter_tests("js_test", selector_config)
    return tests


def get_tests_missing_fcv_tag(tests):
    """Get the list of tests missing requires FCV tag."""
    found_tests = []
    jscomment.get_tags.cache_clear()
    for test in tests:
        try:
            test_tags = jscomment.get_tags(test)
        except FileNotFoundError:
            continue
        else:
            if REQUIRES_FCV_TAG_LATEST not in test_tags:
                found_tests.append(test)
    return found_tests


def main(diff_file, ent_path):
    """Run the main function."""
    with open("base_all_feature_flags.txt", "r") as fh:
        base_feature_flags_turned_on_by_default = fh.read().split()
    with open("patch_all_feature_flags.txt", "r") as fh:
        patch_feature_flags_turned_on_by_default = fh.read().split()
    enabled_feature_flags = [
        flag
        for flag in patch_feature_flags_turned_on_by_default
        if flag not in base_feature_flags_turned_on_by_default
    ]

    if not enabled_feature_flags:
        print(
            "No feature flags were enabled by default in this patch/commit; skipping feature flag checks"
        )
        sys.exit(0)

    tests_with_feature_flag_tag = get_tests_with_feature_flag_tags(enabled_feature_flags, ent_path)

    _run_git_cmd(["apply", diff_file])
    tests_missing_fcv_tag = get_tests_missing_fcv_tag(tests_with_feature_flag_tag)

    if tests_missing_fcv_tag:
        print(
            f"Found tests missing `{REQUIRES_FCV_TAG_LATEST}` tag:\n"
            + "\n".join(tests_missing_fcv_tag)
        )
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--diff-file-name", type=str, help="Name of the file containing the git diff"
    )
    parser.add_argument("--enterprise-path", type=str, help="Path to the enterprise module")
    args = parser.parse_args()
    main(args.diff_file_name, args.enterprise_path)
