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
import logging

LOGGER = logging.getLogger(__name__)

COMMON_PUBLIC_PATTERN = r'''
    ((?P<revert>Revert)\s+[\"\']?)?                         # Revert (optional)
    ((?P<ticket>(?:EVG|SERVER|WT)-[0-9]+)[\"\']?\s*)               # ticket identifier
    (?P<body>(?:(?!\(cherry\spicked\sfrom).)*)?             # To also capture the body
    (?P<backport>\(cherry\spicked\sfrom.*)?                 # back port (optional)
    '''
"""Common Public pattern format."""

COMMON_LINT_PATTERN = r'(?P<lint>Fix\slint)'
"""Common Lint pattern format."""

COMMON_IMPORT_PATTERN = r'(?P<imported>Import\s(wiredtiger|tools):\s.*)'
"""Common Import pattern format."""

COMMON_PRIVATE_PATTERN = r'''
    ((?P<revert>Revert)\s+[\"\']?)?                                     # Revert (optional)
    ((?P<ticket>[A-Z]+-[0-9]+)[\"\']?\s*)                               # ticket identifier
    (?P<body>(?:(?!('\s(into\s'(([^/]+))/(([^:]+)):(([^']+))'))).)*)?   # To also capture the body
'''
"""Common Private pattern format."""

STATUS_OK = 0
STATUS_ERROR = 1

GIT_SHOW_COMMAND = ["git", "show", "-1", "-s", "--format=%s"]


def new_patch_description(pattern: str) -> str:
    """
    Wrap the pattern to conform to the new commit queue patch description format.

    Add the commit queue prefix and suffix to the pattern. The format looks like:

    Commit Queue Merge: '<commit message>' into '<owner>/<repo>:<branch>'

    :param pattern: The pattern to wrap.
    :return: A pattern to match the new format for the patch description.
    """
    return (r"""^((?P<commitqueue>Commit\sQueue\sMerge:)\s')"""
            f'{pattern}'
            # r"""('\s(?P<into>into\s'((?P<owner>[^/]+))/((?P<repo>[^:]+)):((?P<branch>[^']+))'))"""
            )


def old_patch_description(pattern: str) -> str:
    """
    Wrap the pattern to conform to the new commit queue patch description format.

    Just add a start anchor. The format looks like:

    <commit message>

    :param pattern: The pattern to wrap.
    :return: A pattern to match the old format for the patch description.
    """
    return r'^' f'{pattern}'


# NOTE: re.VERBOSE is for visibility / debugging. As such significant white space must be
# escaped (e.g ' ' to \s).
VALID_PATTERNS = [
    re.compile(new_patch_description(COMMON_PUBLIC_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
    re.compile(old_patch_description(COMMON_PUBLIC_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
    re.compile(new_patch_description(COMMON_LINT_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
    re.compile(old_patch_description(COMMON_LINT_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
    re.compile(new_patch_description(COMMON_IMPORT_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
    re.compile(old_patch_description(COMMON_IMPORT_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
]
"""valid public patterns."""

PRIVATE_PATTERNS = [
    re.compile(
        new_patch_description(COMMON_PRIVATE_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
    re.compile(
        old_patch_description(COMMON_PRIVATE_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
]
"""private patterns."""


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
        print('Validating last git commit message')
        result = subprocess.check_output(GIT_SHOW_COMMAND)
        message = result.decode('utf-8')
    else:
        message = " ".join(args.message)

    if any(valid_pattern.match(message) for valid_pattern in VALID_PATTERNS):
        return STATUS_OK
    else:
        if any(private_pattern.match(message) for private_pattern in PRIVATE_PATTERNS):
            error_type = "Found a reference to a private project"
        else:
            error_type = "Found a commit without a ticket"
        LOGGER.error(f"{error_type}\n{message}")  # pylint: disable=logging-fstring-interpolation
        return STATUS_ERROR


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
