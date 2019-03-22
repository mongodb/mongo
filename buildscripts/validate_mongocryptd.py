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
import sys
import yaml

# Name of map to search for in the variables map in evergreen.yml
MONGOCRYPTD_VARIANTS = "mongocryptd_variants"


def main():
    # type: () -> None
    """Execute Main Entry point."""

    parser = argparse.ArgumentParser(description='MongoDB CryptD Check Tool.')

    parser.add_argument('file', type=str, help="etc/evergreen.yml file")

    parser.add_argument('--variant', type=str, help="Build variant to check for")

    args = parser.parse_args()

    # This will raise an exception if the YAML parse fails
    with open(args.file, 'r') as fh:
        nodes = yaml.load(fh)

    variables = nodes["variables"]

    for var in variables:
        if MONGOCRYPTD_VARIANTS in var:
            expected_variants = var[MONGOCRYPTD_VARIANTS]
            break
    else:
        print("ERROR: Could not find node %s in file '%s'" % (MONGOCRYPTD_VARIANTS, args.file),
              file=sys.stderr)
        sys.exit(1)

    if not args.variant in expected_variants:
        print("ERROR: Expected to find variant %s in list %s" % (args.variant, expected_variants),
              file=sys.stderr)
        print("ERROR:  Please add the build variant %s to the %s list in '%s'" %
              (args.variant, MONGOCRYPTD_VARIANTS, args.file), file=sys.stderr)
        sys.exit(1)

    sys.exit(0)


if __name__ == '__main__':
    main()
