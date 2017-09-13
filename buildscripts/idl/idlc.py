#!/usr/bin/env python2
# Copyright (C) 2017 MongoDB Inc.
#
# This program is free software: you can redistribute it and/or  modify
# it under the terms of the GNU Affero General Public License, version 3,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
"""IDL Compiler Driver Main Entry point."""

from __future__ import absolute_import, print_function

import argparse
import logging
import sys

import idl.compiler


def main():
    # type: () -> None
    """Main Entry point."""
    parser = argparse.ArgumentParser(description='MongoDB IDL Compiler.')

    parser.add_argument('file', type=str, help="IDL input file")

    parser.add_argument('-o', '--output', type=str, help="IDL output source file")

    parser.add_argument('--header', type=str, help="IDL output header file")

    parser.add_argument(
        '-i',
        '--include',
        type=str,
        action="append",
        help="Directory to search for IDL import files")

    parser.add_argument('-v', '--verbose', action='count', help="Enable verbose tracing")

    parser.add_argument('--base_dir', type=str, help="IDL output relative base directory")

    parser.add_argument(
        '--write-dependencies',
        action='store_true',
        help='only print out a list of dependent imports')

    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    compiler_args = idl.compiler.CompilerArgs()

    compiler_args.input_file = args.file
    compiler_args.import_directories = args.include

    compiler_args.output_source = args.output
    compiler_args.output_header = args.header
    compiler_args.output_base_dir = args.base_dir
    compiler_args.output_suffix = "_gen"
    compiler_args.write_dependencies = args.write_dependencies

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
