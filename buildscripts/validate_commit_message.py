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
import sys
import logging
from evergreen import RetryingEvergreenApi, EvergreenApi

EVG_CONFIG_FILE = "~/.evergreen.yml"
LOGGER = logging.getLogger(__name__)

ERROR_MSG = """
################################################################################
Encountered an invalid commit message. Please correct to the commit message to
continue. 

Commit message should start with a Public Jira ticket, an "Import" for wiredtiger
or tools, or a "Revert" message.

{error_msg} on '{branch}':
'{commit_message}'
################################################################################
"""

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

COMMON_REVERT_IMPORT_PATTERN = r'Revert\s+[\"\']?(?P<imported>Import\s(wiredtiger|tools):\s.*)'
"""Common revert Import pattern format."""

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
    re.compile(
        new_patch_description(COMMON_REVERT_IMPORT_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
    re.compile(
        old_patch_description(COMMON_REVERT_IMPORT_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
]
"""valid public patterns."""

PRIVATE_PATTERNS = [
    re.compile(
        new_patch_description(COMMON_PRIVATE_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
    re.compile(
        old_patch_description(COMMON_PRIVATE_PATTERN), re.MULTILINE | re.DOTALL | re.VERBOSE),
]
"""private patterns."""


def validate_commit_messages(version_id: str, evg_api: EvergreenApi) -> int:
    """
    Validate the commit messages for the given build.

    :param version_id: ID of version to validate.
    :param evg_api: Evergreen API client.
    :return: True if all commit messages were valid.
    """
    found_error = False
    code_changes = evg_api.patch_by_id(version_id).module_code_changes
    for change in code_changes:
        for message in change.commit_messages:
            if any(valid_pattern.match(message) for valid_pattern in VALID_PATTERNS):
                continue
            elif any(private_pattern.match(message) for private_pattern in PRIVATE_PATTERNS):
                print(
                    ERROR_MSG.format(error_msg="Reference to a private project",
                                     branch=change.branch_name, commit_message=message))
                found_error = True
            else:
                print(
                    ERROR_MSG.format(error_msg="Commit without a ticket", branch=change.branch_name,
                                     commit_message=message))
                found_error = True

    return STATUS_ERROR if found_error else STATUS_OK


def main(argv=None):
    """Execute Main function to validate commit messages."""
    parser = argparse.ArgumentParser(
        usage="Validate the commit message. "
        "It validates the latest message when no arguments are provided.")
    parser.add_argument(
        "version_id",
        metavar="version id",
        help="The id of the version to validate",
    )
    parser.add_argument("--evg-config-file", default=EVG_CONFIG_FILE,
                        help="Path to evergreen configuration file containing auth information.")
    args = parser.parse_args(argv)
    evg_api = RetryingEvergreenApi.get_api(config_file=os.path.expanduser(args.evg_config_file))

    return validate_commit_messages(args.version_id, evg_api)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
