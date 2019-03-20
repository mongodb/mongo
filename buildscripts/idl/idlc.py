#!/usr/bin/env python2
#
# Copyright (C) 2018-present MongoDB, Inc.
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
"""IDL Compiler Driver Main Entry point."""

from __future__ import absolute_import, print_function

import argparse
import logging
import sys

import idl.compiler


def main():
    # type: () -> None
    """Execute Main Entry point."""
    parser = argparse.ArgumentParser(description='MongoDB IDL Compiler.')

    parser.add_argument('file', type=str, help="IDL input file")

    parser.add_argument('-o', '--output', type=str, help="IDL output source file")

    parser.add_argument('--header', type=str, help="IDL output header file")

    parser.add_argument('-i', '--include', type=str, action="append",
                        help="Directory to search for IDL import files")

    parser.add_argument('-v', '--verbose', action='count', help="Enable verbose tracing")

    parser.add_argument('--base_dir', type=str, help="IDL output relative base directory")

    parser.add_argument('--write-dependencies', action='store_true',
                        help='only print out a list of dependent imports')

    parser.add_argument('--write-dependencies-inline', action='store_true',
                        help='print out a list of dependent imports during file generation')

    parser.add_argument('--target_arch', type=str,
                        help="IDL target archiecture (amd64, s390x). defaults to current machine")

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    compiler_args = idl.compiler.CompilerArgs()

    compiler_args.input_file = args.file
    compiler_args.import_directories = args.include
    compiler_args.target_arch = args.target_arch

    compiler_args.output_source = args.output
    compiler_args.output_header = args.header
    compiler_args.output_base_dir = args.base_dir
    compiler_args.output_suffix = "_gen"
    compiler_args.write_dependencies = args.write_dependencies
    compiler_args.write_dependencies_inline = args.write_dependencies_inline

    if (args.output is not None and args.header is None) or \
        (args.output is  None and args.header is not None):
        print("ERROR: Either both --header and --output must be specified or neither.")
        sys.exit(1)

    # Compile the IDL document the user specified
    success = idl.compiler.compile_idl(compiler_args)

    if not success:
        sys.exit(1)


if __name__ == '__main__':
    main()
