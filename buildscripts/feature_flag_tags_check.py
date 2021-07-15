#!/usr/bin/env python3
"""Feature flag tags check.

Check that on changing feature flag from disabled to enabled by default in all js tests that
had that feature flag in tags there is a tag that requires the latest FCV.
"""

import os
import subprocess
import sys

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.resmokelib import selector
from buildscripts.resmokelib.multiversionconstants import LATEST_FCV
from buildscripts.resmokelib.utils import jscomment

REQUIRES_FCV_TAG = f"requires_fcv_{LATEST_FCV}".replace(".", "")
ENTERPRISE_DIR = "src/mongo/db/modules/enterprise"


def _run_git_stash_cmd(args, cwd=None):
    """Run git stash command."""
    git_cmd = ["git", "stash"] + args
    proc = subprocess.Popen(git_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd)
    proc.communicate()


def _git_stash(args):
    """Run git stash command in he current and enterprise directory."""
    _run_git_stash_cmd(args)
    if os.path.isdir(ENTERPRISE_DIR):
        _run_git_stash_cmd(args, cwd=ENTERPRISE_DIR)


def get_tests_with_feature_flag_tags(feature_flags):
    """Get the list of tests with feature flag tag."""
    selector_config = {
        "roots": ["jstests/**/*.js", f"{ENTERPRISE_DIR}/jstests/**/*.js"],
        "include_with_any_tags": feature_flags,
    }
    tests, _ = selector.filter_tests("js_test", selector_config)
    return tests


def get_tests_missing_fcv_tag(tests):
    """Get the list of tests missing requires FCV tag."""
    found_tests = []
    for test in tests:
        try:
            test_tags = jscomment.get_tags(test)
        except FileNotFoundError:
            continue
        else:
            if REQUIRES_FCV_TAG not in test_tags:
                found_tests.append(test)
    return found_tests


def main():
    """Run the main function."""
    with open("base_all_feature_flags.txt", "r") as fh:
        base_feature_flags = fh.read().split()
    with open("patch_all_feature_flags.txt", "r") as fh:
        patch_feature_flags = fh.read().split()
    enabled_feature_flags = [flag for flag in base_feature_flags if flag not in patch_feature_flags]

    if not enabled_feature_flags:
        sys.exit(0)

    _git_stash(["--", "jstests"])
    tests_with_feature_flag_tag = get_tests_with_feature_flag_tags(enabled_feature_flags)
    _git_stash(["pop"])
    tests_missing_fcv_tag = get_tests_missing_fcv_tag(tests_with_feature_flag_tag)

    if tests_missing_fcv_tag:
        print(f"Found tests missing `{REQUIRES_FCV_TAG}` tag:\n" + "\n".join(tests_missing_fcv_tag))
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
