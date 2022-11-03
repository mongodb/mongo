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
import sys
import re
import shutil
from subprocess import check_output
from typing import List
from packaging.version import Version

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

# pylint: disable=wrong-import-position
from buildscripts.resmokelib.multiversionconstants import LAST_LTS_FCV, LAST_CONTINUOUS_FCV, LATEST_FCV
# pylint: enable=wrong-import-position

LOGGER_NAME = 'checkout-idl'
LOGGER = logging.getLogger(LOGGER_NAME)


def get_release_tags() -> List[str]:
    """Get a list of release git tags since API Version 1 was introduced."""

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
        """Get the latest released tag for LATEST, LAST_CONTINUOUS and LAST_LTS versions."""

        base_versions = [Version(x) for x in [LAST_LTS_FCV, LAST_CONTINUOUS_FCV, LATEST_FCV]]
        min_version = None
        max_version = None

        for version, tag in sorted(gen_versions_and_tags(), reverse=True):
            if version.is_prerelease:
                continue

            while min_version is None or version < min_version:
                # If we encounter a version smaller than the current min,
                # replace the current min with FCV of the next reachable version.
                #
                # e.g. if we are looking for latest release of LAST_CONTINUOS_FCV (6.2.x
                # and we encounter (6.1.13), update min/max version to match
                # the latest release of LAST_LTS_FCV (6.0.x).

                if not len(base_versions) > 0:
                    return
                min_version = base_versions.pop()
                max_version = Version("{}.{}".format(min_version.major, min_version.minor + 1))

            if version >= max_version:
                continue

            yield tag
            min_version = None

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
