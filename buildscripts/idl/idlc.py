#!/usr/bin/env python3
# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""IDL Compiler Driver Main Entry point."""

import argparse
import logging
import sys

import idl.compiler


def main():
    # type: () -> None
    """Execute Main Entry point."""
    parser = argparse.ArgumentParser(description="MongoDB IDL Compiler.")

    parser.add_argument("file", type=str, help="IDL input file")

    parser.add_argument("-o", "--output", type=str, help="IDL output source file")

    parser.add_argument("--header", type=str, help="IDL output header file")

    parser.add_argument(
        "-i",
        "--include",
        type=str,
        action="append",
        help="Directory to search for IDL import files",
    )

    parser.add_argument("-v", "--verbose", action="count", help="Enable verbose tracing")

    parser.add_argument("--base_dir", type=str, help="IDL output relative base directory")

    parser.add_argument(
        "--write-dependencies",
        action="store_true",
        help="only print out a list of dependent imports",
    )

    parser.add_argument(
        "--write-dependencies-inline",
        action="store_true",
        help="print out a list of dependent imports during file generation",
    )

    parser.add_argument(
        "--target_arch",
        type=str,
        help="IDL target archiecture (amd64, s390x). defaults to current machine",
    )

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

    if (args.output is not None and args.header is None) or (
        args.output is None and args.header is not None
    ):
        print("ERROR: Either both --header and --output must be specified or neither.")
        sys.exit(1)

    # Compile the IDL document the user specified
    success = idl.compiler.compile_idl(compiler_args)

    if not success:
        sys.exit(1)


if __name__ == "__main__":
    main()
