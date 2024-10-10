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
import shutil
import sys
from subprocess import check_output
from typing import List

from packaging.version import Version

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

# pylint: disable=wrong-import-position
from buildscripts.resmokelib.multiversionconstants import (
    LAST_CONTINUOUS_FCV,
    LAST_LTS_FCV,
    LATEST_FCV,
)

# pylint: enable=wrong-import-position

LOGGER_NAME = "checkout-idl"
LOGGER = logging.getLogger(LOGGER_NAME)


def get_tags() -> List[str]:
    """Get a list of git tags that the IDL compatibility script should check against."""

    def gen_versions_and_tags():
        for tag in check_output(["git", "tag"]).decode().split():
            # Releases are like "r5.6.7". Older ones aren't r-prefixed but we don't care about them.
            if not tag.startswith("r"):
                continue

            try:
                yield Version(tag[1:]), tag
            except ValueError:
                # Not a release tag.
                pass

    def gen_tags():
        """
        Get the latest released tag for LATEST, LAST_CONTINUOUS and LAST_LTS versions.

        If the version is not yet released, get the latest unreleased tag for that version.
        """

        fcvs = [Version(x) for x in [LAST_LTS_FCV, LAST_CONTINUOUS_FCV, LATEST_FCV]]
        # Remove duplicates from our generic FCVs list. The potential duplicates are when last
        # LTS is the same as last continuous.
        fcvs = [*set(fcvs)]

        # Initialize a results dict that points each FCV to a tuple of
        # (candidate_tag, is_prerelease_version).
        results = {fcv: (None, True) for fcv in fcvs}

        # For each generic FCV, this algorithm fetches the latest released tag for that version. If
        # there are no released versions for a FCV, this algorithm fetches the tag for the latest
        # unreleased version.
        for version, tag in sorted(gen_versions_and_tags(), reverse=True):
            major_minor_version = Version(f"{version.major}.{version.minor}")
            if major_minor_version in results:
                candidate_tag, candidate_is_prerelease_version = results[major_minor_version]
                if candidate_tag is None:
                    # This is the first tag we have seen for this version. Set our first
                    # candidate tag and if this tag is a prerelease version.
                    results[major_minor_version] = (tag, version.is_prerelease)
                elif candidate_is_prerelease_version and not version.is_prerelease:
                    # This version is the first released version we have seen and our previous
                    # candidate tag is not a released version. Set the tag to point to this
                    # version instead.
                    results[major_minor_version] = (tag, version.is_prerelease)

        for tag, _ in results.values():
            yield tag

    return list(gen_tags())


def make_idl_directories(tags: List[str], destination: str) -> None:
    """For each tag, construct a source tree containing only its IDL files."""
    LOGGER.info("Clearing destination directory '%s'", destination)
    shutil.rmtree(destination, ignore_errors=True)

    for tag in tags:
        LOGGER.info("Checking out IDL files in %s", tag)
        directory = os.path.join(destination, tag)
        for path in check_output(["git", "ls-tree", "--name-only", "-r", tag]).decode().split():
            if not path.endswith(".idl"):
                continue

            contents = check_output(["git", "show", f"{tag}:{path}"]).decode()
            output_path = os.path.join(directory, path)
            os.makedirs(os.path.dirname(output_path), exist_ok=True)
            with open(output_path, "w+") as fd:
                fd.write(contents)


def main():
    """Run the script."""
    arg_parser = argparse.ArgumentParser(description=__doc__)
    arg_parser.add_argument("-v", "--verbose", action="count", help="Enable verbose logging")
    arg_parser.add_argument(
        "destination", metavar="DESTINATION", help="Directory to check out past IDL file versions"
    )
    args = arg_parser.parse_args()

    logging.basicConfig(level=logging.WARNING)
    logging.getLogger(LOGGER_NAME).setLevel(logging.DEBUG if args.verbose else logging.INFO)

    tags = get_tags()
    LOGGER.info("Fetching IDL files for past tags: %s", tags)
    assert len(tags) >= 2, "we must always have at least two tags to check"
    make_idl_directories(tags, args.destination)


if __name__ == "__main__":
    main()
