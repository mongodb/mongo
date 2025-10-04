# Copyright (C) 2020-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
"""
Generate a file containing a list of disabled feature flags.

Used by resmoke.py to run only feature flag tests.
"""

import os
import sys
from typing import List

import typer
import yaml

# Permit imports from "buildscripts".
sys.path.append(os.path.normpath(os.path.join(os.path.abspath(__file__), "../../..")))

from buildscripts.idl import lib
from buildscripts.idl.idl import binder, parser


def get_all_feature_flags(idl_dirs: List[str] = None):
    """Generate a dict of all feature flags with their default value."""
    default_idl_dirs = ["src", "buildscripts"]

    if not idl_dirs:
        idl_dirs = default_idl_dirs

    all_flags = {}
    for idl_dir in idl_dirs:
        for idl_path in sorted(lib.list_idls(idl_dir)):
            if lib.is_third_party_idl(idl_path):
                continue
            # Most IDL files do not contain feature flags.
            # We can discard these quickly without expensive YAML parsing.
            with open(idl_path) as idl_file:
                if "feature_flags" not in idl_file.read():
                    continue
            with open(idl_path) as idl_file:
                doc = parser.parse_file(idl_file, idl_path)
            for feature_flag in doc.spec.feature_flags:
                all_flags[feature_flag.name] = feature_flag

    return all_flags


def get_all_feature_flags_turned_on_by_default(idl_dirs: List[str] = None):
    """Generate a list of all feature flags that default to true."""
    all_flags = get_all_feature_flags(idl_dirs)

    return [
        name for name, flag in all_flags.items() if binder.is_feature_flag_enabled_by_default(flag)
    ]


def get_all_feature_flags_turned_off_by_default(idl_dirs: List[str] = None):
    """Generate a list of all feature flags that default to false."""
    all_flags = get_all_feature_flags(idl_dirs)
    all_default_false_flags = [
        name
        for name, flag in all_flags.items()
        if not binder.is_feature_flag_enabled_by_default(flag)
    ]

    with open(
        "buildscripts/resmokeconfig/fully_disabled_feature_flags.yml", encoding="utf8"
    ) as fully_disabled_ffs:
        force_disabled_flags = yaml.safe_load(fully_disabled_ffs)

    return list(set(all_default_false_flags) - set(force_disabled_flags))


def get_all_unreleased_ifr_feature_flags(idl_dirs: List[str] = None):
    """Generate a list of all features flags in the 'in_development' incremental rollout phase."""
    all_flags = get_all_feature_flags(idl_dirs)

    return [
        name
        for name, flag in all_flags.items()
        if binder.is_unreleased_incremental_rollout_feature_flag(flag)
    ]


def write_feature_flags_to_file(flags: List[str], filename: str):
    """Helper function to write feature flags to a file."""
    with open(filename, "w") as output_file:
        output_file.write("\n".join(flags))
        print(f"Generated: {os.path.realpath(output_file.name)}")


cli = typer.Typer()


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
    all_flags = get_all_feature_flags()

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
