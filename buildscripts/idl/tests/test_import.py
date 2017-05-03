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
"""Test cases for IDL binder."""

from __future__ import absolute_import, print_function, unicode_literals

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
        # type: (Dict[unicode, unicode]) -> None
        """Construct a DictionaryImportResolver."""
        self._import_dict = import_dict
        super(DictionaryImportResolver, self).__init__()

    def resolve(self, base_file, imported_file_name):
        # type: (unicode, unicode) -> unicode
        """Return the complete path to an imported file name."""
        # pylint: disable=unused-argument
        if not imported_file_name in self._import_dict:
            return None

        return "imported_%s" % (imported_file_name)

    def open(self, resolved_file_name):
        # type: (unicode) -> Any
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
            """),
            resolver=resolver)

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
            """),
            resolver=resolver)

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
            """),
            resolver=resolver)

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
            """),
            resolver=resolver)

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
            """),
            resolver=resolver)

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
            """)
        }

        resolver = DictionaryImportResolver(import_dict)

        # Bad import
        self.assert_parse_fail(
            textwrap.dedent("""
        imports:
            - "notfound.idl"
            """),
            idl.errors.ERROR_ID_BAD_IMPORT,
            resolver=resolver)

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
            """),
            idl.errors.ERROR_ID_DUPLICATE_SYMBOL,
            resolver=resolver)

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
            """),
            idl.errors.ERROR_ID_DUPLICATE_SYMBOL,
            resolver=resolver)

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
            """),
            idl.errors.ERROR_ID_DUPLICATE_SYMBOL,
            resolver=resolver)

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
            """),
            idl.errors.ERROR_ID_DUPLICATE_SYMBOL,
            resolver=resolver)


if __name__ == '__main__':

    unittest.main()
