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
import os
import re
import subprocess
import sys

VALID_PATTERNS = [
    re.compile(r"^Fix lint$"),  # Allow "Fix lint" as the sole commit summary
    re.compile(r'^(Revert ["\']?)?(EVG|SERVER|WT)-[0-9]+'),  # These are public tickets
    re.compile(r'^Import (wiredtiger|tools):'),  # These are public tickets
]
PRIVATE_PATTERNS = [re.compile(r"^[A-Z]+-[0-9]+")]

STATUS_OK = 0
STATUS_ERROR = 1

GIT_SHOW_COMMAND = ["git", "show", "-1", "-s", "--format=%s"]


def main(argv=None):
    """Execute Main function to validate commit messages."""
    parser = argparse.ArgumentParser(
        usage="Validate the commit message. "
        "It validates the latest message when no arguments are provided.")
    parser.add_argument(
        "-i",
        action="store_true",
        dest="ignore_warnings",
        help="Ignore all warnings.",
        default=False,
    )
    parser.add_argument(
        "message",
        metavar="commit message",
        nargs="*",
        help="The commit message to validate",
    )
    args = parser.parse_args(argv)

    if not args.message:
        print('Validating last git commit message')
        result = subprocess.check_output(GIT_SHOW_COMMAND)
        message = result.decode('utf-8')
    else:
        message = " ".join(args.message)

    if any(valid_pattern.match(message) for valid_pattern in VALID_PATTERNS):
        status = STATUS_OK
    elif any(private_pattern.match(message) for private_pattern in PRIVATE_PATTERNS):
        print("ERROR: found a reference to a private project\n{message}".format(message=message))
        status = STATUS_ERROR
    else:
        print("{message_type}: found a commit without a ticket\n{message}".format(
            message_type="WARNING" if args.ignore_warnings else "ERROR", message=message))
        status = STATUS_OK if args.ignore_warnings else STATUS_ERROR
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
