# This script is used as a workspace status command
#    bazel build --workspace_status_command="python bazel/workspace_status.py" ...
# to populate key-value pairs in bazel-out/volatile-status.txt and stable-status.txt.
# These files and the key-values can be consumed by bazel rules, but bazel
# pretends volatile-status never changes when deciding what to rebuild.
#
# Note on imports and python versions:
#   This file is executed by Bazel prior to the build. Expect no packages to
#   be available except those installed by wrapper_hook.py
import json
import os
import pathlib
import re
import sys
import urllib.request

# Prepend repository root to path, to avoid shadowing from bazel runfiles.
REPO_ROOT = str(pathlib.Path(__file__).parent.parent)
sys.path.insert(0, REPO_ROOT)
from bazel.wrapper_hook.install_modules import setup_python_path


def print_stable_key(key: str, value: str):
    """Prints a stable workspace status key-value pair."""
    print(f"STABLE_{key} {value}")


def print_volatile_key(key: str, value: str):
    """Prints a volatile workspace status key-value pair."""
    print(f"{key} {value}")


def print_feature_flag_status():
    setup_python_path()
    try:
        from buildscripts.idl import lib as idl_lib
        from buildscripts.idl.idl import binder as idl_binder
    except ModuleNotFoundError:
        # Modules will not be installed by wrapper_hook.py during the
        # builds it performs internally (lint, compiledb). In these
        # situations, these volatile keys can simply be skipped.
        return
    all_flags = idl_lib.get_all_feature_flags()

    enabled_flags = [
        name
        for name, flag in all_flags.items()
        if idl_binder.is_feature_flag_enabled_by_default(flag)
    ]
    print_stable_key("on_feature_flags", " ".join(enabled_flags))

    disabled_flags = [
        name
        for name, flag in all_flags.items()
        if not idl_binder.is_feature_flag_enabled_by_default(flag)
    ]
    print_stable_key("off_feature_flags", " ".join(disabled_flags))

    unreleased_ifr_flags = [
        name
        for name, flag in all_flags.items()
        if idl_binder.is_unreleased_incremental_rollout_feature_flag(flag)
    ]
    print_stable_key("unreleased_ifr_flags", " ".join(unreleased_ifr_flags))


def print_test_runtimes_status():
    project = os.environ.get("project", "")
    build_variant = os.environ.get("build_variant", "")
    task = os.environ.get("task_name", "")
    if not project or not build_variant or not task:
        return

    # Extracts the base task name, i.e. auth_0-linux_enterprise -> auth
    match = re.search(r"(.+)_\d+-(linux|mac|windows)*", task)
    if match:
        task = match.group(1)

    url = f"https://mongo-test-stats.s3.amazonaws.com/{project}/{build_variant}/{task}"
    try:
        with urllib.request.urlopen(url) as response:
            content = response.read()
            stats = json.loads(content)
    except Exception:
        return

    for s in stats:
        for field in ["num_pass", "num_fail", "max_duration_pass"]:
            s.pop(field, None)
    print_volatile_key("TEST_RUNTIMES", json.dumps(stats))


def print_evergreen_expansions():
    for expansion in [
        "build_id",
        "distro_id",
        "execution",
        "project",
        "revision",
        "revision_order_id",
        "task_id",
        "task_name",
        "build_variant",
        "version_id",
        "requester",
    ]:
        value = os.environ.get(expansion, "")
        if value:
            print_volatile_key(expansion, value)


def main():
    print_evergreen_expansions()
    print_feature_flag_status()
    print_test_runtimes_status()


if __name__ == "__main__":
    main()
