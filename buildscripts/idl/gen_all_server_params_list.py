# Copyright (C) 2023-present MongoDB, Inc.
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
Generate a file containing a list of all available server parameters.

Used by DSI to conditionally allow configuration of internalQueryStatsRateLimit parameter.
"""

import os
import sys
from typing import List

# Permit imports from "buildscripts".
sys.path.append(os.path.normpath(os.path.join(os.path.abspath(__file__), "../../..")))

# pylint: disable=wrong-import-position
from buildscripts.idl import lib
from buildscripts.idl.idl import parser


def gen_all_server_params(idl_dirs: List[str] = None):
    """Generate a list of all server parameters."""
    default_idl_dirs = ["src"]

    if not idl_dirs:
        idl_dirs = default_idl_dirs

    all_params = []
    for idl_dir in idl_dirs:
        for idl_path in sorted(lib.list_idls(idl_dir)):
            if lib.is_third_party_idl(idl_path):
                continue
            # Most IDL files do not contain server parameters.
            # We can discard these quickly without expensive YAML parsing.
            with open(idl_path) as idl_file:
                if "server_parameters" not in idl_file.read():
                    continue
            with open(idl_path) as idl_file:
                doc = parser.parse_file(idl_file, idl_path)
            for server_param in doc.spec.server_parameters:
                all_params.append(server_param.name)

    return all_params


def gen_all_server_params_file(filename: str = "all_server_params.txt"):
    flags = gen_all_server_params()
    with open(filename, "w") as output_file:
        output_file.write("\n".join(flags))
        print("Generated: ", os.path.realpath(output_file.name))


def main():
    """Run the main function."""
    gen_all_server_params_file()


if __name__ == "__main__":
    main()
