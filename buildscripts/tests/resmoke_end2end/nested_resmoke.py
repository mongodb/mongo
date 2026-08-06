"""Helpers for the end2end tests, which spawn nested resmoke invocations."""

import os
import shutil

# Where the Bazel resmoke_suite_test rule stages the version file in the runfiles tree.
_BAZEL_MONGO_VERSION_FILE = os.path.join("bazel", "resmoke", ".resmoke_mongo_version.yml")

_MONGO_VERSION_FILE = ".resmoke_mongo_version.yml"

# The repo root as this module sees it, which is the RESMOKE_ROOT of a nested invocation
# started by absolute path (the tests that run resmoke from another directory).
_MODULE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(__file__))))


def stage_mongo_version_file():
    """Put .resmoke_mongo_version.yml where the nested resmoke invocations look for it.

    resmoke generates this file itself, but only when run from the root of the repo.
    Under Bazel the working directory is the test's output directory instead, so copy
    over the version file Bazel staged in the runfiles tree. A nested invocation
    resolves the file against its own RESMOKE_ROOT, which is the working directory when
    it is started as `buildscripts/resmoke.py` and the runfiles tree when it is started
    by absolute path, so seed both.
    """
    if not os.path.exists(_BAZEL_MONGO_VERSION_FILE):
        return

    roots = {os.path.abspath(os.curdir), os.path.abspath(_MODULE_ROOT)}
    for root in roots:
        dest = os.path.join(root, _MONGO_VERSION_FILE)
        if not os.path.exists(dest):
            shutil.copyfile(_BAZEL_MONGO_VERSION_FILE, dest)
