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
"""
IDL compiler driver.

Orchestrates the 3 passes (parser, binder, and generator) together.
"""

import io
import logging
import os
import platform
from typing import Any, List

from . import binder
from . import errors
from . import generator
from . import parser
from . import syntax


class CompilerArgs(object):
    """Set of compiler arguments."""

    def __init__(self):
        # type: () -> None
        """Create a container for compiler arguments."""
        self.import_directories = None  # type: List[str]
        self.input_file = None  # type: str
        self.target_arch = None  # type: str

        self.output_source = None  # type: str
        self.output_header = None  # type: str
        self.output_base_dir = None  # type: str
        self.output_suffix = None  # type: str

        self.write_dependencies = False  # type: bool
        self.write_dependencies_inline = False  # type: bool


class CompilerImportResolver(parser.ImportResolverBase):
    """Class for the IDL compiler to resolve imported files."""

    def __init__(self, import_directories):
        # type: (List[str]) -> None
        """Construct a ImportResolver."""
        self._import_directories = import_directories

        super(CompilerImportResolver, self).__init__()

    def resolve(self, base_file, imported_file_name):
        # type: (str, str) -> str
        """Return the complete path to an imported file name."""

        logging.debug("Resolving imported file '%s' for file '%s'", imported_file_name, base_file)

        # Check for fully-qualified paths
        logging.debug("Checking for imported file '%s' for file '%s' at '%s'", imported_file_name,
                      base_file, imported_file_name)
        if os.path.isabs(imported_file_name) and os.path.exists(imported_file_name):
            logging.debug("Found imported file '%s' for file '%s' at '%s'", imported_file_name,
                          base_file, imported_file_name)
            return imported_file_name

        for candidate_dir in self._import_directories or []:
            base_dir = os.path.abspath(candidate_dir)
            resolved_file_name = os.path.normpath(os.path.join(base_dir, imported_file_name))

            logging.debug("Checking for imported file '%s' for file '%s' at '%s'",
                          imported_file_name, base_file, resolved_file_name)

            if os.path.exists(resolved_file_name):
                logging.debug("Found imported file '%s' for file '%s' at '%s'", imported_file_name,
                              base_file, resolved_file_name)
                return resolved_file_name

        msg = ("Cannot find imported file '%s' for file '%s'" % (imported_file_name, base_file))
        logging.error(msg)

        raise errors.IDLError(msg)

    def open(self, resolved_file_name):
        # type: (str) -> Any
        """Return an io.Stream for the requested file."""
        return io.open(resolved_file_name, encoding='utf-8')


def _write_dependencies(spec, write_dependencies_inline):
    # type: (syntax.IDLSpec, bool) -> None
    """Write a list of dependencies to standard out."""
    if not spec.imports:
        return

    dependencies = sorted(spec.imports.dependencies)
    for resolved_file_name in dependencies:
        if write_dependencies_inline:
            resolved_file_name = "import file:" + resolved_file_name

        print(resolved_file_name)


def _update_import_includes(args, spec, header_file_name):
    # type: (CompilerArgs, syntax.IDLSpec, str) -> None
    """Update the list of imports with a list of include files for each import with structs."""
    # This function is fragile:
    # In order to try to generate headers with an "include what you use" set of headers, the IDL
    # compiler needs to generate include statements to headers for imported files with structs. The
    # problem is that the IDL compiler needs to make the following assumptions:
    # 1. The layout of build vs source directory.
    # 2. The file naming suffix rules for all IDL invocations are consistent.
    if not spec.imports:
        return

    if args.output_base_dir:
        base_include_h_file_name = os.path.relpath(
            os.path.normpath(header_file_name), os.path.normpath(args.output_base_dir))
    else:
        base_include_h_file_name = os.path.abspath(header_file_name)

    # Normalize to POSIX style for consistency across Windows and POSIX.
    base_include_h_file_name = base_include_h_file_name.replace("\\", "/")

    # Modify the includes list of the root_doc to include all of its direct imports
    if not spec.globals:
        spec.globals = syntax.Global(args.input_file, -1, -1)

    first_dir = base_include_h_file_name.split('/')[0]

    for resolved_file_name in spec.imports.resolved_imports:
        # Guess: the file naming rules are consistent across IDL invocations
        include_h_file_name = os.path.splitext(resolved_file_name)[0] + args.output_suffix + ".h"

        if args.output_base_dir:
            base_dir = os.path.normpath(args.output_base_dir)
            include_h_file_name = os.path.relpath(os.path.normpath(include_h_file_name), base_dir)

            if os.path.isabs(base_dir):
                include_h_file_name = os.path.join(
                    base_dir, include_h_file_name[include_h_file_name.rfind(first_dir):])
            else:
                include_h_file_name = include_h_file_name[include_h_file_name.find(first_dir):]
        else:
            include_h_file_name = os.path.abspath(include_h_file_name)

        # Normalize to POSIX style for consistency across Windows and POSIX.
        include_h_file_name = include_h_file_name.replace("\\", "/")

        spec.globals.cpp_includes.append(include_h_file_name)


def compile_idl(args):
    # type: (CompilerArgs) -> bool
    """Compile an IDL file into C++ code."""
    # Named compile_idl to avoid naming conflict with builtin
    if not os.path.exists(args.input_file):
        logging.error("File '%s' not found", args.input_file)

    if args.output_source is None:
        if not '.' in args.input_file:
            logging.error("File name '%s' must be end with a filename extension, such as '%s.idl'",
                          args.input_file, args.input_file)
            return False

        file_name_prefix = os.path.splitext(args.input_file)[0]
        file_name_prefix += args.output_suffix

        source_file_name = file_name_prefix + ".cpp"
        header_file_name = file_name_prefix + ".h"
    else:
        source_file_name = args.output_source
        header_file_name = args.output_header

    if args.target_arch is None:
        args.target_arch = platform.machine()

    # Compile the IDL through the 3 passes
    with io.open(args.input_file, encoding='utf-8') as file_stream:
        parsed_doc = parser.parse(file_stream, args.input_file,
                                  CompilerImportResolver(args.import_directories))

        if not parsed_doc.errors:
            if args.write_dependencies or args.write_dependencies_inline:
                _write_dependencies(parsed_doc.spec, args.write_dependencies_inline)

                # Stop compiling if we only need to scan import dependencies
                if args.write_dependencies:
                    return True

            _update_import_includes(args, parsed_doc.spec, header_file_name)

            bound_doc = binder.bind(parsed_doc.spec)
            if not bound_doc.errors:
                generator.generate_code(bound_doc.spec, args.target_arch, args.output_base_dir,
                                        header_file_name, source_file_name)

                return True
            else:
                bound_doc.errors.dump_errors()
        else:
            parsed_doc.errors.dump_errors()

        return False
