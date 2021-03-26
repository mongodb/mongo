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
"""
Generate a file containing a list of disabled feature flags.

Used by resmoke.py to run only feature flag tests.
"""

import argparse
import os
import sys

from typing import List

import yaml

# Permit imports from "buildscripts".
sys.path.append(os.path.normpath(os.path.join(os.path.abspath(__file__), '../../..')))

# pylint: disable=wrong-import-position
import buildscripts.idl.lib as lib


def gen_all_feature_flags(idl_dir: str, import_dirs: List[str]):
    """Generate a list of all feature flags."""
    all_flags = []
    for idl_path in sorted(lib.list_idls(idl_dir)):
        for feature_flag in lib.parse_idl(idl_path, import_dirs).spec.feature_flags:
            if feature_flag.default.literal != "true":
                all_flags.append(feature_flag.name)

    force_disabled_flags = yaml.safe_load(
        open("buildscripts/resmokeconfig/fully_disabled_feature_flags.yml"))

    return list(set(all_flags) - set(force_disabled_flags))


def main():
    """Run the main function."""
    arg_parser = argparse.ArgumentParser(description=__doc__)
    arg_parser.add_argument("--import-dir", dest="import_dirs", type=str, action="append",
                            help="Directory to search for IDL import files")

    args = arg_parser.parse_args()

    flags = gen_all_feature_flags(os.getcwd(), args.import_dirs)
    with open(lib.ALL_FEATURE_FLAG_FILE, "w") as output_file:
        for flag in flags:
            output_file.write("%s\n" % flag)


if __name__ == '__main__':
    main()
