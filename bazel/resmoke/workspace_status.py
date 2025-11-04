# Note on imports and python versions:
#   This file is executed by Bazel using the system Python interpreter. Expect
#   no packages to be available except those in the standard library. Try to
#   avoid Python version specific code.
import json
import os
import re
import sys
import urllib.request

sys.path.append(os.path.normpath(os.path.join(os.path.abspath(__file__), "../../..")))
from buildscripts.idl import lib as idl_lib
from buildscripts.idl.idl import binder as idl_binder


def print_stable_key(key: str, value: str):
    """Prints a stable workspace status key-value pair."""
    print(f"STABLE_{key} {value}")


def print_volatile_key(key: str, value: str):
    """Prints a volatile workspace status key-value pair."""
    print(f"{key} {value}")


def print_feature_flag_status():
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


def main():
    print_feature_flag_status()
    print_test_runtimes_status()


if __name__ == "__main__":
    main()
