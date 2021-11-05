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
"""Check out previous releases' IDL files, in preparation for checking backwards compatibility."""

import argparse
import logging
import os
import re
import shutil
from subprocess import check_output
from typing import List

from packaging.version import Version

FIRST_API_V1_RELEASE = '5.0.0-rc3'
LOGGER_NAME = 'checkout-idl'
LOGGER = logging.getLogger(LOGGER_NAME)


def get_current_git_version() -> Version:
    """Return current git version'.

    If the version is a release like "2.3.4" or "2.3.4-rc0", or a pre-release like
    "2.3.4-325-githash" or "2.3.4-pre-" these will return "2.3.4". If the version begins with the
    letter 'r', it will also match, e.g. r2.3.4, r2.3.4-rc0, r2.3.4-git234, r2.3.4-rc0-234-githash
    If the version is invalid (i.e. doesn't start with "2.3.4" or "2.3.4-rc0", this will return
    False.
    """
    git_describe = check_output(['git', 'describe']).decode()
    git_version = re.match(r'^r?(\d+\.\d+\.\d+)(?:-rc\d+|-alpha\d+)?(?:-.*)?', git_describe)
    assert git_version, f"git describe output '{git_describe}' does not match pattern."
    return Version(git_version.groups()[0])


def get_release_tags() -> List[str]:
    """Get a list of release git tags since API Version 1 was introduced."""
    # Use packaging.version.Version's parsing and comparison logic.
    min_version = Version(FIRST_API_V1_RELEASE)
    max_version = get_current_git_version()

    def gen_versions_and_tags():
        for tag in check_output(['git', 'tag']).decode().split():
            # Releases are like "r5.6.7". Older ones aren't r-prefixed but we don't care about them.
            if not tag.startswith('r'):
                continue

            try:
                yield Version(tag[1:]), tag
            except ValueError:
                # Not a release tag.
                pass

    def gen_release_tags():
        #  gen_versions_and_tags yields pairs (version, tag). Sort them by version using
        #  packaging.version.Version's comparison rules.
        for version, tag in sorted(gen_versions_and_tags()):
            if version < min_version or version > max_version:
                continue

            # Skip alphas, betas, etc.
            if version.is_prerelease and version != min_version:
                continue

            yield tag

    return list(gen_release_tags())


def make_idl_directories(release_tags: List[str], destination: str) -> None:
    """For each release, construct a source tree containing only its IDL files."""
    LOGGER.info("Clearing destination directory '%s'", destination)
    shutil.rmtree(destination, ignore_errors=True)

    for tag in release_tags:
        LOGGER.info("Checking out IDL files in %s", tag)
        directory = os.path.join(destination, tag)
        for path in check_output(['git', 'ls-tree', '--name-only', '-r', tag]).decode().split():
            if not path.endswith('.idl'):
                continue

            contents = check_output(['git', 'show', f'{tag}:{path}']).decode()
            output_path = os.path.join(directory, path)
            os.makedirs(os.path.dirname(output_path), exist_ok=True)
            with open(output_path, 'w+') as fd:
                fd.write(contents)


def main():
    """Run the script."""
    arg_parser = argparse.ArgumentParser(description=__doc__)
    arg_parser.add_argument("-v", "--verbose", action="count", help="Enable verbose logging")
    arg_parser.add_argument("destination", metavar="DESTINATION",
                            help="Directory to check out past IDL file versions")
    args = arg_parser.parse_args()

    logging.basicConfig(level=logging.WARNING)
    logging.getLogger(LOGGER_NAME).setLevel(logging.DEBUG if args.verbose else logging.INFO)

    tags = get_release_tags()
    LOGGER.debug("Fetching IDL files for %s past releases", len(tags))
    make_idl_directories(tags, args.destination)


if __name__ == "__main__":
    main()
