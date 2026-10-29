# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""
Generate a file containing a list of disabled feature flags.

Used by resmoke.py to run only feature flag tests.
"""

import os
import sys

import typer
import yaml

# Permit imports from "buildscripts".
sys.path.append(os.path.normpath(os.path.join(os.path.abspath(__file__), "../../..")))

from buildscripts.idl import lib
from buildscripts.idl.idl import binder
from buildscripts.resmokelib import config as resmoke_config

DEFAULT_IDL_DIRS = [
    os.path.join(resmoke_config.RESMOKE_ROOT, "src"),
    os.path.join(resmoke_config.RESMOKE_ROOT, "buildscripts"),
]


def get_all_feature_flags_turned_on_by_default(idl_dirs: list[str] = DEFAULT_IDL_DIRS):
    """Generate a list of all feature flags that default to true."""
    all_flags = lib.get_all_feature_flags(idl_dirs)

    return [
        name for name, flag in all_flags.items() if binder.is_feature_flag_enabled_by_default(flag)
    ]


def get_all_feature_flags_turned_off_by_default(idl_dirs: list[str] = DEFAULT_IDL_DIRS):
    """Generate a list of all feature flags that default to false."""
    all_flags = lib.get_all_feature_flags(idl_dirs)
    all_default_false_flags = [
        name
        for name, flag in all_flags.items()
        if not binder.is_feature_flag_enabled_by_default(flag)
    ]

    fully_disabled_path = os.path.join(
        resmoke_config.RESMOKE_ROOT, "buildscripts/resmokeconfig/fully_disabled_feature_flags.yml"
    )
    with open(fully_disabled_path, encoding="utf8") as fully_disabled_ffs:
        force_disabled_flags = yaml.safe_load(fully_disabled_ffs)

    return list(set(all_default_false_flags) - set(force_disabled_flags))


def get_all_unreleased_ifr_feature_flags(idl_dirs: list[str] = DEFAULT_IDL_DIRS):
    """Generate a list of all features flags in the 'in_development' incremental rollout phase."""
    all_flags = lib.get_all_feature_flags(idl_dirs)

    return [
        name
        for name, flag in all_flags.items()
        if binder.is_unreleased_incremental_rollout_feature_flag(flag)
    ]


def get_all_ifr_feature_flags(idl_dirs: list[str] = DEFAULT_IDL_DIRS):
    """Generate a list of all IFR feature flags regardless of phase."""
    all_flags = lib.get_all_feature_flags(idl_dirs)

    return [
        name for name, flag in all_flags.items() if binder.is_incremental_feature_rollout_flag(flag)
    ]


def write_feature_flags_to_file(flags: list[str], filename: str):
    """Helper function to write feature flags to a file."""
    with open(filename, "w") as output_file:
        output_file.write("\n".join(flags))
        print(f"Generated: {os.path.realpath(output_file.name)}")


cli = typer.Typer(pretty_exceptions_show_locals=False)


@cli.command("turned-off-by-default")
def turned_off_by_default(filename: str = "all_feature_flags.txt"):
    """Generate a list of feature flags that default to OFF."""
    flags = get_all_feature_flags_turned_off_by_default()
    write_feature_flags_to_file(flags, filename)


@cli.command("turned-on-by-default")
def turned_on_by_default(filename: str = "all_feature_flags.txt"):
    """Generate a list of feature flags that default to ON."""
    flags = get_all_feature_flags_turned_on_by_default()
    write_feature_flags_to_file(flags, filename)


@cli.command("feature-flag-status")
def feature_flag_status():
    """Generate lists of all default-enabled feature flags, all default-disabled feature flags and
    all 'in_development' IFR feature flags, with each list on its own line
    """
    all_flags = lib.get_all_feature_flags()

    default_enabled_flags = [
        name for name, flag in all_flags.items() if binder.is_feature_flag_enabled_by_default(flag)
    ]
    print(f"on_feature_flags {' '.join(default_enabled_flags)}")

    default_disabled_flags = [
        name
        for name, flag in all_flags.items()
        if not binder.is_feature_flag_enabled_by_default(flag)
    ]
    print(f"off_feature_flags {' '.join(default_disabled_flags)}")

    unreleased_ifr_flags = [
        name
        for name, flag in all_flags.items()
        if binder.is_unreleased_incremental_rollout_feature_flag(flag)
    ]
    print(f"unreleased_ifr_flags {' '.join(unreleased_ifr_flags)}")


if __name__ == "__main__":
    cli()
