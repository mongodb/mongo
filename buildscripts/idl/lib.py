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
"""Library functions and utility methods used across user-facing IDL scripts."""

import os
from typing import List, Set

from buildscripts.idl.idl import parser, syntax
from buildscripts.idl.idl.compiler import CompilerImportResolver


def list_idls(directory: str) -> Set[str]:
    """Find all IDL files in the current directory."""
    return {
        os.path.join(dirpath, filename)
        for dirpath, dirnames, filenames in os.walk(directory)
        for filename in filenames
        if filename.endswith(".idl")
    }


def parse_idl(idl_path: str, import_directories: List[str]) -> syntax.IDLParsedSpec:
    """Parse an IDL file or throw an error."""
    parsed_doc = parser.parse(open(idl_path), idl_path, CompilerImportResolver(import_directories))

    if parsed_doc.errors:
        parsed_doc.errors.dump_errors()
        raise ValueError(f"Cannot parse {idl_path}")

    return parsed_doc


def is_third_party_idl(idl_path: str) -> bool:
    """Check if an IDL file is under a third party directory."""
    third_party_idl_subpaths = [os.path.join("third_party", "mozjs"), "win32com"]

    for file_name in third_party_idl_subpaths:
        if file_name in idl_path:
            return True

    return False
