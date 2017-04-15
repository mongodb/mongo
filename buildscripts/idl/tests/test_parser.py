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
"""Test cases for IDL parser."""

from __future__ import absolute_import, print_function, unicode_literals

import textwrap
import unittest

# import package so that it works regardless of whether we run as a module or file
if __package__ is None:
    import sys
    from os import path
    sys.path.append(path.dirname(path.dirname(path.abspath(__file__))))
    from context import idl
    import testcase
else:
    from .context import idl
    from . import testcase


class TestParser(testcase.IDLTestcase):
    """Test the IDL parser only."""

    def test_empty(self):
        # type: () -> None
        """Test an empty document works."""
        self.assert_parse("")

    def test_root_negative(self):
        # type: () -> None
        """Test unknown root scalar fails."""
        self.assert_parse_fail(
            textwrap.dedent("""
        fake:
            cpp_namespace: 'foo'
            """), idl.errors.ERROR_ID_UNKNOWN_ROOT)

    def test_global_positive(self):
        # type: () -> None
        """Postive global tests."""
        # cpp_namespace alone
        self.assert_parse(textwrap.dedent("""
        global:
            cpp_namespace: 'foo'"""))

        # cpp_includes scalar
        self.assert_parse(textwrap.dedent("""
        global:
            cpp_includes: 'foo'"""))

        # cpp_includes list
        self.assert_parse(
            textwrap.dedent("""
        global:
            cpp_includes: 
                - 'bar'
                - 'foo'"""))

    def test_global_negative(self):
        # type: () -> None
        """Negative global tests."""

        # Global is a scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        global: foo
            """), idl.errors.ERROR_ID_IS_NODE_TYPE)

        # Duplicate globals
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_namespace: 'foo'
        global:
            cpp_namespace: 'bar'
            """), idl.errors.ERROR_ID_DUPLICATE_NODE)

        # Duplicate cpp_namespace
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_namespace: 'foo'
            cpp_namespace: 'foo'"""), idl.errors.ERROR_ID_DUPLICATE_NODE)

        # Duplicate cpp_includes
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_includes: 'foo'
            cpp_includes: 'foo'"""), idl.errors.ERROR_ID_DUPLICATE_NODE)

        # cpp_namespace as a sequence
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_namespace:
                - 'foo'
                - 'bar'"""), idl.errors.ERROR_ID_IS_NODE_TYPE)

        # cpp_namespace as a map
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_namespace:
                name: 'foo'"""), idl.errors.ERROR_ID_IS_NODE_TYPE)

        # cpp_includes as a map
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_includes:
                inc1: 'foo'"""), idl.errors.ERROR_ID_IS_NODE_TYPE_SCALAR_OR_SEQUENCE)

        # Unknown scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            bar: 'foo'
            """), idl.errors.ERROR_ID_UNKNOWN_NODE)

    def test_type_positive(self):
        # type: () -> None
        """Positive type test cases."""

        # Test all positive fields works
        self.assert_parse(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                cpp_type: foo
                bson_serialization_type: foo
                serializer: foo
                deserializer: foo
                default: foo
                bindata_subtype: foo
            """))

        # Test sequence of bson serialization types
        self.assert_parse(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                cpp_type: foo
                bson_serialization_type:
                    - foo
                    - bar
            """))

    def test_type_negative(self):
        # type: () -> None
        """Negative type test cases."""

        # Test duplicate types
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                description: test
                cpp_type: foo
                bson_serialization_type: int
        types:
            bar:
                description: test
                cpp_type: foo
                bson_serialization_type: int
                """), idl.errors.ERROR_ID_DUPLICATE_NODE)

        # Test scalar fails
        self.assert_parse_fail(
            textwrap.dedent("""
            types:
                foo: 'bar'"""), idl.errors.ERROR_ID_IS_NODE_TYPE)

        # Test unknown field
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                bogus: foo
                description: test
                cpp_type: foo
                bson_serialization_type:
                """), idl.errors.ERROR_ID_UNKNOWN_NODE)

        # test duplicate field
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                description: test
                cpp_type: foo
                bson_serialization_type:
                """), idl.errors.ERROR_ID_DUPLICATE_NODE)

        # test list instead of scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            - foo:
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            multiple=True)

        # test list instead of scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                - bar
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            multiple=True)

        # test map instead of scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                description:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            multiple=True)

        # test missing bson_serialization_type field
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                cpp_type: foo
            """), idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD)

        # test missing cpp_type field
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                bson_serialization_type: foo
            """), idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD)

    def test_struct_positive(self):
        # type: () -> None
        """Positive struct test cases."""

        # All fields with true for bools
        self.assert_parse(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: true
                fields:
                    foo: bar
            """))

        # All fields with false for bools
        self.assert_parse(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                fields:
                    foo: bar
            """))

    def test_struct_negative(self):
        # type: () -> None
        """Negative struct test cases."""

        # Missing fields
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: true
            """), idl.errors.ERROR_ID_EMPTY_FIELDS)

        # unknown field
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                foo: bar
                fields:
                    foo: bar
            """), idl.errors.ERROR_ID_UNKNOWN_NODE)

        # strict is a bool
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: bar
                fields:
                    foo: bar
            """), idl.errors.ERROR_ID_IS_NODE_VALID_BOOL)

    def test_field_positive(self):
        # type: () -> None
        """Positive field test cases."""

        # Test short types
        self.assert_parse(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    foo: short
            """))

        # Test all fields
        self.assert_parse(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    foo:
                        type: foo
                        description: foo
                        optional: true
                        ignore: true
            """))

        # Test false bools
        self.assert_parse(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
                        optional: false
                        ignore: false
            """))

    def test_field_negative(self):
        # type: () -> None
        """Negative field test cases."""

        # Test duplicate fields
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                fields:
                    foo: short
                    foo: int
            """), idl.errors.ERROR_ID_DUPLICATE_NODE)

        # Test bad bool
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
                        optional: bar
            """), idl.errors.ERROR_ID_IS_NODE_VALID_BOOL)

        # Test bad bool
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
                        ignore: bar
            """), idl.errors.ERROR_ID_IS_NODE_VALID_BOOL)

    def test_name_collisions_negative(self):
        # type: () -> None
        """Negative tests for type collisions."""
        # Struct after type
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo1:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo

        structs:
            foo1:
                description: foo
                strict: true
                fields:
                    foo: string
            """), idl.errors.ERROR_ID_DUPLICATE_SYMBOL)

        # Type after struct
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo1:
                description: foo
                strict: true
                fields:
                    foo: string

        types:
            foo1:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo
            """), idl.errors.ERROR_ID_DUPLICATE_SYMBOL)


if __name__ == '__main__':

    unittest.main()
