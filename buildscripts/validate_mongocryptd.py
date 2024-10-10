# Copyright (C) 2019-present MongoDB, Inc.
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
#
"""Validate that mongocryptd push tasks are correct in etc/evergreen.yml."""

from __future__ import absolute_import, print_function, unicode_literals

import argparse
import os
import sys

import yaml

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.ciconfig.evergreen import parse_evergreen_file

# pylint: enable=wrong-import-position

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
    with open(filename, "r") as fh:
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
