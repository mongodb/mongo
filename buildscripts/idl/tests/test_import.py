#!/usr/bin/env python3
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
"""Test cases for IDL binder."""

import io
import textwrap
import unittest
from typing import Any, Dict

# import package so that it works regardless of whether we run as a module or file
if __package__ is None:
    import sys
    from os import path
    sys.path.append(path.dirname(path.abspath(__file__)))
    from context import idl
    import testcase
else:
    from .context import idl
    from . import testcase


class DictionaryImportResolver(idl.parser.ImportResolverBase):
    """An import resolver resolves files from a dictionary."""

    def __init__(self, import_dict):
        # type: (Dict[str, str]) -> None
        """Construct a DictionaryImportResolver."""
        self._import_dict = import_dict
        super(DictionaryImportResolver, self).__init__()

    def resolve(self, base_file, imported_file_name):
        # type: (str, str) -> str
        """Return the complete path to an imported file name."""
        # pylint: disable=unused-argument
        if not imported_file_name in self._import_dict:
            return None

        return "imported_%s" % (imported_file_name)

    def open(self, resolved_file_name):
        # type: (str) -> Any
        """Return an io.Stream for the requested file."""
        assert resolved_file_name.startswith("imported_")
        imported_file_name = resolved_file_name.replace("imported_", "")

        return io.StringIO(self._import_dict[imported_file_name])


class TestImport(testcase.IDLTestcase):
    """Test cases for the IDL binder."""

    # Test: import wrong types
    def test_import_negative_parser(self):
        # type: () -> None
        """Negative import parser tests."""

        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "a.idl"

        imports:
            - "b.idl"
            """), idl.errors.ERROR_ID_DUPLICATE_NODE)

        self.assert_parse_fail(
            textwrap.dedent("""
        imports: "basetypes.idl"
            """), idl.errors.ERROR_ID_IS_NODE_TYPE)

        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            a: "a.idl"
            b: "b.idl"
            """), idl.errors.ERROR_ID_IS_NODE_TYPE)

    def test_import_positive(self):
        # type: () -> None
        """Postive import tests."""

        import_dict = {
            "basetypes.idl":
                textwrap.dedent("""
            global:
                cpp_namespace: 'something'

            types:
                string:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo
                    default: foo

            structs:
                bar:
                    description: foo
                    strict: false
                    fields:
                        foo: string
            """),
            "recurse1.idl":
                textwrap.dedent("""
            imports:
                - "basetypes.idl"

            types:
                int:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: int
                    deserializer: BSONElement::fake
            """),
            "recurse2.idl":
                textwrap.dedent("""
            imports:
                - "recurse1.idl"

            types:
                double:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: double
                    deserializer: BSONElement::fake
            """),
            "recurse1b.idl":
                textwrap.dedent("""
            imports:
                - "basetypes.idl"

            types:
                bool:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bool
                    deserializer: BSONElement::fake
            """),
            "cycle1a.idl":
                textwrap.dedent("""
            global:
                cpp_namespace: 'something'

            imports:
                - "cycle1b.idl"

            types:
                string:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo
                    default: foo

            structs:
                bar:
                    description: foo
                    strict: false
                    fields:
                        foo: string
                        foo1: bool
            """),
            "cycle1b.idl":
                textwrap.dedent("""
            global:
                cpp_namespace: 'something'

            imports:
                - "cycle1a.idl"

            types:
                bool:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bool
                    deserializer: BSONElement::fake

            structs:
                bar2:
                    description: foo
                    strict: false
                    fields:
                        foo: string
                        foo1: bool
            """),
            "cycle2.idl":
                textwrap.dedent("""
            global:
                cpp_namespace: 'something'

            imports:
                - "cycle2.idl"

            types:
                string:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo
                    default: foo
            """),
        }

        resolver = DictionaryImportResolver(import_dict)

        # Test simple import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'something'

        imports:
            - "basetypes.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
            """), resolver=resolver)

        # Test nested import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'something'

        imports:
            - "recurse2.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
                    foo1: int
                    foo2: double
            """), resolver=resolver)

        # Test diamond import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'something'

        imports:
            - "recurse2.idl"
            - "recurse1b.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
                    foo1: int
                    foo2: double
                    foo3: bool
            """), resolver=resolver)

        # Test cycle import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'something'

        imports:
            - "cycle1a.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
                    foo1: bool
            """), resolver=resolver)

        # Test self cycle import
        self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'something'

        imports:
            - "cycle2.idl"

        structs:
            foobar:
                description: foo
                strict: false
                fields:
                    foo: string
            """), resolver=resolver)

    def test_import_negative(self):
        # type: () -> None
        """Negative import tests."""

        import_dict = {
            "basetypes.idl":
                textwrap.dedent("""
            global:
                cpp_namespace: 'something'

            types:
                string:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo
                    default: foo

            structs:
                bar:
                    description: foo
                    strict: false
                    fields:
                        foo: string

            enums:
                IntEnum:
                    description: "An example int enum"
                    type: int
                    values:
                        a0: 0
                        b1: 1

            """),
            "bug.idl":
                textwrap.dedent("""
            global:
                cpp_namespace: 'something'

            types:
                bool:
                    description: foo
                    bson_serialization_type: bool
                    deserializer: BSONElement::fake
            """),
        }

        resolver = DictionaryImportResolver(import_dict)

        # Bad import
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "notfound.idl"
            """), idl.errors.ERROR_ID_BAD_IMPORT, resolver=resolver)

        # Duplicate types
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "basetypes.idl"

        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
            """), idl.errors.ERROR_ID_DUPLICATE_SYMBOL, resolver=resolver)

        # Duplicate structs
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "basetypes.idl"

        structs:
            bar:
                description: foo
                fields:
                    foo1: string
            """), idl.errors.ERROR_ID_DUPLICATE_SYMBOL, resolver=resolver)

        # Duplicate struct and type
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "basetypes.idl"

        structs:
            string:
                description: foo
                fields:
                    foo1: string
            """), idl.errors.ERROR_ID_DUPLICATE_SYMBOL, resolver=resolver)

        # Duplicate type and struct
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "basetypes.idl"

        types:
            bar:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
            """), idl.errors.ERROR_ID_DUPLICATE_SYMBOL, resolver=resolver)

        # Duplicate enums
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "basetypes.idl"

        enums:
            IntEnum:
                description: "An example int enum"
                type: int
                values:
                    a0: 0
                    b1: 1
            """), idl.errors.ERROR_ID_DUPLICATE_SYMBOL, resolver=resolver)

        # Import a file with errors
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "basetypes.idl"
            - "bug.idl"

        types:
            string2:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
            """), idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD, resolver=resolver)


if __name__ == '__main__':

    unittest.main()
