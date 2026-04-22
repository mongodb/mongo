# This script is used as a workspace status command
#    bazel build --workspace_status_command="python bazel/workspace_status.py" ...
# to populate key-value pairs in bazel-out/volatile-status.txt and stable-status.txt.
# These files and the key-values can be consumed by bazel rules, but bazel
# pretends volatile-status never changes when deciding what to rebuild.
#
# Note on imports and python versions:
#   This file is executed by Bazel prior to the build. Expect no packages to
#   be available except those installed by wrapper_hook.py
import os
import pathlib
import sys

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

    all_ifr_flags = [
        name
        for name, flag in all_flags.items()
        if idl_binder.is_incremental_feature_rollout_flag(flag)
    ]
    print_stable_key("all_ifr_flags", " ".join(all_ifr_flags))


def print_evergreen_expansions():
    for expansion in [
        "build_id",
        "build_variant",
        "distro_id",
        "execution",
        "otel_parent_id",
        "otel_trace_id",
        "project",
        "requester",
        "revision",
        "revision_order_id",
        "task_id",
        "task_name",
        "version_id",
    ]:
        value = os.environ.get(expansion, "")
        if value:
            print_volatile_key(expansion, value)


def main():
    print_evergreen_expansions()
    print_feature_flag_status()


if __name__ == "__main__":
    main()
