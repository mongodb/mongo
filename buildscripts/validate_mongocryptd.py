# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Validate that mongocryptd push tasks are correct in etc/evergreen.yml."""

from __future__ import absolute_import, print_function, unicode_literals

import argparse
import os
import sys

import yaml

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.ciconfig.evergreen import parse_evergreen_file

# Name of map to search for in the variables map in evergreen.yml
MONGOCRYPTD_VARIANTS = "mongocryptd_variants"
PUSH_TASK_NAME = "push"


def can_validation_be_skipped(evg_config, variant):
    """
    Determine if the given build variant needs to be validated.

    A build variant does not need to be validated if it does not run the 'push' task or
    if it does not exist in the configuration (it is dynamically created).

    :param evg_config: Evergreen configuration.
    :param variant: Build variant to check.
    :return: True if validation can be skipped.
    """
    variant_config = evg_config.get_variant(variant)
    if not variant_config:
        return True

    if PUSH_TASK_NAME not in variant_config.task_names:
        return True

    return False


def read_variable_from_yml(filename, variable_name):
    """
    Read the given variable from the given yaml file.

    :param filename: Yaml file to read from.
    :param variable_name: Variable to read from file.
    :return: Value of variable or None.
    """
    with open(filename, "r", encoding="utf8") as fh:
        nodes = yaml.safe_load(fh)

    variables = nodes["variables"]

    for var in variables:
        if variable_name in var:
            return var[variable_name]
    return None


def main():
    # type: () -> None
    """Execute Main Entry point."""

    parser = argparse.ArgumentParser(description="MongoDB CryptD Check Tool.")

    parser.add_argument("file", type=str, help="etc/evergreen.yml file")
    parser.add_argument("--variant", type=str, help="Build variant to check for")

    args = parser.parse_args()

    expected_variants = read_variable_from_yml(args.file, MONGOCRYPTD_VARIANTS)
    if not expected_variants:
        print(
            "ERROR: Could not find node %s in file '%s'" % (MONGOCRYPTD_VARIANTS, args.file),
            file=sys.stderr,
        )
        sys.exit(1)

    evg_config = parse_evergreen_file(args.file)
    if can_validation_be_skipped(evg_config, args.variant):
        print(f"Skipping validation on buildvariant {args.variant}")
        sys.exit(0)

    if args.variant not in expected_variants:
        print(
            "ERROR: Expected to find variant %s in list %s" % (args.variant, expected_variants),
            file=sys.stderr,
        )
        print(
            "ERROR:  Please add the build variant %s to the %s list in '%s'"
            % (args.variant, MONGOCRYPTD_VARIANTS, args.file),
            file=sys.stderr,
        )
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
