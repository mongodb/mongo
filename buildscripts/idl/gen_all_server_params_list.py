# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""
Generate a file containing a list of all available server parameters.

Used by DSI to conditionally allow configuration of internalQueryStatsRateLimit parameter.
"""

import os
import sys

# Permit imports from "buildscripts".
sys.path.append(os.path.normpath(os.path.join(os.path.abspath(__file__), "../../..")))

from buildscripts.idl import lib
from buildscripts.idl.idl import parser


def gen_all_server_params(idl_dirs: list[str] = None):
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
