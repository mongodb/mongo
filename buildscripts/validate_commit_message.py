#!/usr/bin/env python3
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
"""Validate that the commit message is ok."""
import argparse
import logging
import re
import sys

LOGGER = logging.getLogger(__name__)

STATUS_OK = 0
STATUS_ERROR = 1


def main(argv=None):
    """Execute Main function to validate commit messages."""
    parser = argparse.ArgumentParser(
        usage="Validate the commit message. "
        "It validates the latest message when no arguments are provided.")
    parser.add_argument(
        "message",
        metavar="commit message",
        nargs="*",
        help="The commit message to validate",
    )
    args = parser.parse_args(argv)

    if not args.message:
        LOGGER.error("Must specify non-empty value for --message")
        return STATUS_ERROR
    message = " ".join(args.message)

    # Valid values look like:
    # 1. SERVER-\d+
    # 2. Revert "SERVER-\d+
    # 3. Import wiredtiger
    # 4. Revert "Import wiredtiger
    valid_pattern = re.compile(r'(Revert ")?(SERVER-[0-9]+|Import wiredtiger)')

    if valid_pattern.match(message):
        return STATUS_OK
    else:
        LOGGER.error(f"Found a commit without a ticket\n{message}")  # pylint: disable=logging-fstring-interpolation
        return STATUS_ERROR


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
