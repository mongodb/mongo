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

import textwrap
import unittest

# import package so that it works regardless of whether we run as a module or file
if __package__ is None:
    import sys
    from os import path

    sys.path.append(path.dirname(path.abspath(__file__)))
    import testcase
    from context import idl
else:
    from . import testcase
    from .context import idl

# All YAML tests assume 4 space indent
INDENT_SPACE_COUNT = 4


def fill_spaces(count):
    # type: (int) -> str
    """Fill a string full of spaces."""
    fill = ""
    for _ in range(count * INDENT_SPACE_COUNT):
        fill += " "

    return fill


def indent_text(count, unindented_text):
    # type: (int, str) -> str
    """Indent each line of a multi-line string."""
    lines = unindented_text.splitlines()
    fill = fill_spaces(count)
    return "\n".join(fill + line for line in lines)


class TestBinder(testcase.IDLTestcase):
    """Test cases for the IDL binder."""

    # Create a text wrap for common types.
    common_types = textwrap.dedent("""
    types:
        object:
            description: foo
            cpp_type: foo
            bson_serialization_type: object
            serializer: foo
            deserializer: foo
            is_view: false

        bool:
            description: foo
            cpp_type: foo
            bson_serialization_type: any
            serializer: foo
            deserializer: foo
            is_view: false

        string:
            description: foo
            cpp_type: foo
            bson_serialization_type: string
            serializer: foo
            deserializer: foo
            is_view: false

        any_type:
            description: foo
            cpp_type: foo
            bson_serialization_type: any
            serializer: foo
            deserializer: foo
            is_view: false

        tenant_id:
            bson_serialization_type: any
            description: foo
            cpp_type: foo
            deserializer: foo
            serializer: foo
            is_view: false

        database_name:
            bson_serialization_type: string
            description: foo
            cpp_type: foo
            serializer: foo
            deserializer: foo
            is_view: false

        serialization_context:
            bson_serialization_type: any
            description: foo
            cpp_type: foo
            internal_only: true
            is_view: false
    """)

    def test_empty(self):
        # type: () -> None
        """Test an empty document works."""
        self.assert_bind("")

    def test_global_positive(self):
        # type: () -> None
        """Postive global tests."""
        spec = self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'mongo'
            cpp_includes:
                - 'bar'
                - 'foo'""")
        )
        self.assertEqual(spec.globals.cpp_namespace, "mongo")
        self.assertListEqual(spec.globals.cpp_includes, ["bar", "foo"])

        spec = self.assert_bind(
            textwrap.dedent("""
        global:
            cpp_namespace: 'mongo::nested'
        """)
        )
        self.assertEqual(spec.globals.cpp_namespace, "mongo::nested")

    def test_global_negatives(self):
        # type: () -> None
        """Postive global tests."""
        self.assert_bind_fail(
            textwrap.dedent("""
        global:
            cpp_namespace: 'something'
        """),
            idl.errors.ERROR_ID_BAD_CPP_NAMESPACE,
        )

    def test_type_positive(self):
        # type: () -> None
        """Positive type tests."""
        self.assert_bind(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo
                is_view: false
            """)
        )

        # Test supported types
        for bson_type in [
            "bool",
            "date",
            "null",
            "decimal",
            "double",
            "int",
            "long",
            "objectid",
            "regex",
            "string",
            "timestamp",
            "undefined",
        ]:
            self.assert_bind(
                textwrap.dedent(
                    """
        types:
            foofoo:
                description: foo
                cpp_type: foo
                bson_serialization_type: %s
                default: foo
                deserializer: BSONElement::fake
                is_view: false
            """
                    % (bson_type)
                )
            )

        # Test supported numeric types
        for cpp_type in [
            "std::int32_t",
            "std::uint32_t",
            "std::int32_t",
            "std::uint64_t",
            "std::vector<std::uint8_t>",
            "std::array<std::uint8_t, 16>",
        ]:
            self.assert_bind(
                textwrap.dedent(
                    """
                types:
                    foofoo:
                        description: foo
                        cpp_type: %s
                        bson_serialization_type: int
                        deserializer: BSONElement::fake
                        is_view: false
                """
                    % (cpp_type)
                )
            )

        # Test object
        self.assert_bind(
            textwrap.dedent("""
        types:
            foofoo:
                description: foo
                cpp_type: foo
                bson_serialization_type: object
                serializer: foo
                deserializer: foo
                default: foo
                is_view: false
            """)
        )

        # Test 'any'
        self.assert_bind(
            textwrap.dedent("""
        types:
            foofoo:
                description: foo
                cpp_type: foo
                bson_serialization_type: any
                serializer: foo
                deserializer: foo
                default: foo
                is_view: false
            """)
        )

        # Test 'chain'
        self.assert_bind(
            textwrap.dedent("""
        types:
            foofoo:
                description: foo
                cpp_type: foo
                bson_serialization_type: chain
                serializer: foo
                deserializer: foo
                default: foo
                is_view: false
            """)
        )

        # Test supported bindata_subtype
        for bindata_subtype in ["generic", "function", "uuid", "md5"]:
            self.assert_bind(
                textwrap.dedent(
                    """
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: %s
                    deserializer: BSONElement::fake
                    is_view: false
                """
                    % (bindata_subtype)
                )
            )

    def test_type_negative(self):
        # type: () -> None
        """Negative type tests for properties that types and fields share."""

        # Test bad bson type name
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: foo
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_TYPE,
        )

        # Test bad cpp_type name
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: StringData
                    bson_serialization_type: string
                    deserializer: bar
                    is_view: false
            """),
            idl.errors.ERROR_ID_NO_STRINGDATA,
        )

        # Test unsupported serialization
        for cpp_type in [
            "char",
            "signed char",
            "unsigned char",
            "signed short int",
            "short int",
            "short",
            "signed short",
            "unsigned short",
            "unsigned short int",
            "signed int",
            "signed",
            "unsigned int",
            "unsigned",
            "signed long int",
            "signed long",
            "int",
            "long int",
            "long",
            "unsigned long int",
            "unsigned long",
            "signed long long int",
            "signed long long",
            "long long int",
            "long long",
            "unsigned long int",
            "unsigned long",
            "wchar_t",
            "char16_t",
            "char32_t",
            "int8_t",
            "int16_t",
            "int32_t",
            "int64_t",
            "uint8_t",
            "uint16_t",
            "uint32_t",
            "uint64_t",
        ]:
            self.assert_bind_fail(
                textwrap.dedent(
                    """
                types:
                    foofoo:
                        description: foo
                        cpp_type: %s
                        bson_serialization_type: int
                        deserializer: BSONElement::int
                        is_view: false
                    """
                    % (cpp_type)
                ),
                idl.errors.ERROR_ID_BAD_NUMERIC_CPP_TYPE,
            )

        # Test the std prefix 8 and 16-byte integers fail
        for std_cpp_type in ["std::int8_t", "std::int16_t", "std::uint8_t", "std::uint16_t"]:
            self.assert_bind_fail(
                textwrap.dedent(
                    """
                types:
                    foofoo:
                        description: foo
                        cpp_type: %s
                        bson_serialization_type: int
                        deserializer: BSONElement::int
                        is_view: false
                    """
                    % (std_cpp_type)
                ),
                idl.errors.ERROR_ID_BAD_NUMERIC_CPP_TYPE,
            )

        # Test bindata_subtype missing
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    deserializer: BSONElement::fake
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE,
        )

        # Test fake bindata_subtype is wrong
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: foo
                    deserializer: BSONElement::fake
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE,
        )

        # Test deprecated bindata_subtype 'binary', and 'uuid_old' are wrong
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: binary
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE,
        )

        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: uuid_old
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE,
        )

        # Test bindata_subtype on wrong type
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    bindata_subtype: generic
                    deserializer: BSONElement::fake
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_TYPE,
        )

        # Test bindata with default
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: bindata
                    bindata_subtype: uuid
                    default: 42
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BINDATA_DEFAULT,
        )

        # Test bindata in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - bindata
                                - string
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_TYPE,
        )

        # Test bindata in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: StringData
                    bson_serialization_type:
                                - bindata
                                - string
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_TYPE,
        )

        # Test 'any' in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - any
                                - int
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_ANY_TYPE_USE,
        )

        # Test object in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - object
                                - int
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_TYPE_LIST,
        )

        # Test fake in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - int
                                - fake
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_BSON_TYPE,
        )

        # Test 'chain' in list of types
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type:
                                - chain
                                - int
                    is_view: false
            """),
            idl.errors.ERROR_ID_BAD_ANY_TYPE_USE,
        )

        # Test 'any' serialization needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: any
                    is_view: false
            """),
            idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD,
        )

        # Test 'chain' serialization needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: chain
                    serializer: bar
                    is_view: false
            """),
            idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD,
        )

        # Test 'string' serialization needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: bar
                    is_view: false
            """),
            idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD,
        )

        # Test 'date' serialization needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: date
                    is_view: false
            """),
            idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD,
        )

        # Test 'chain' serialization needs serializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: chain
                    deserializer: bar
                    is_view: false
            """),
            idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD,
        )

        # Test list of bson types needs deserializer
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                foofoo:
                    description: foo
                    cpp_type: std::int32_t
                    bson_serialization_type:
                                - int
                                - string
                    is_view: false
            """),
            idl.errors.ERROR_ID_MISSING_AST_REQUIRED_FIELD,
        )

        # Test array as name
        self.assert_bind_fail(
            textwrap.dedent("""
            types:
                array<foo>:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    deserializer: bar
                    is_view: false
            """),
            idl.errors.ERROR_ID_ARRAY_NOT_VALID_TYPE,
        )

    def test_struct_positive(self):
        # type: () -> None
        """Positive struct tests."""

        # Setup some common types
        test_preamble = self.common_types + indent_text(
            1,
            textwrap.dedent("""
            int:
                description: foo
                cpp_type: std::int32_t
                bson_serialization_type: int
                deserializer: mongo::BSONElement::_numberInt
                is_view: false
        """),
        )

        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: string
            """)
        )

        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: array<int>
            """)
        )

    def test_struct_negative(self):
        # type: () -> None
        """Negative struct tests."""

        # Setup some common types
        test_preamble = self.common_types + indent_text(
            1,
            textwrap.dedent("""
            int:
                description: foo
                cpp_type: std::int32_t
                bson_serialization_type: int
                deserializer: mongo::BSONElement::_numberInt
                is_view: false
        """),
        )

        # Test array as name
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                array<foo>:
                    description: foo
                    strict: true
                    fields:
                        foo: string
            """),
            idl.errors.ERROR_ID_ARRAY_NOT_VALID_TYPE,
        )

        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: array<int>
            """)
        )

    def test_variant_positive(self):
        # type: () -> None
        """Positive variant test cases."""

        # Setup some common types
        test_preamble = self.common_types + indent_text(
            1,
            textwrap.dedent("""
            int:
                description: foo
                cpp_type: std::int32_t
                bson_serialization_type: int
                deserializer: mongo::BSONElement::_numberInt
                is_view: false
            bindata_function:
                bson_serialization_type: bindata
                bindata_subtype: function
                description: "A BSON bindata of function sub type"
                cpp_type: "std::vector<std::uint8_t>"
                deserializer: "mongo::BSONElement::_binDataVector"
                is_view: false
        """),
        )

        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - string
                            - int
            """)
        )

        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - string
                            - bindata_function
            """)
        )

        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - string
                            - int
                        default: 1
            """)
        )

        # Test multiple BSON serialization type Object.
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        structs:
            insert_type:
                description: foo
                fields: {insert: string}
            update_type:
                description: foo
                fields: {update: int}
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - insert_type
                            - update_type
                            - int
            """)
        )

    def test_variant_negative(self):
        # type: () -> None
        """Negative variant test cases."""

        # Setup some common types
        test_preamble = (
            self.common_types
            + indent_text(
                1,
                textwrap.dedent("""
            int:
                description: foo
                cpp_type: std::int32_t
                bson_serialization_type: int
                deserializer: mongo::BSONElement::_numberInt
                is_view: false
            safeInt:
                bson_serialization_type:
                - long
                - int
                - decimal
                - double
                description: foo
                cpp_type: "std::int32_t"
                deserializer: "mongo::BSONElement::safeNumberInt"
                is_view: false
        """),
            )
            + textwrap.dedent("""
        enums:
            foo_enum:
                description: foo
                type: int
                values:
                    v1: 0
                    v2: 1
        """)
        )

        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - string
            """),
            idl.errors.ERROR_ID_USELESS_VARIANT,
        )

        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - string
                            - int
                            - not_defined
            """),
            idl.errors.ERROR_ID_UNKNOWN_TYPE,
            True,
        )

        # Enums are banned in variants for now.
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - string
                            - foo_enum
            """),
            idl.errors.ERROR_ID_NO_VARIANT_ENUM,
            True,
        )

        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - string
                            - string
            """),
            idl.errors.ERROR_ID_VARIANT_DUPLICATE_TYPES,
        )

        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - array<string>
                            - array<string>
            """),
            idl.errors.ERROR_ID_VARIANT_DUPLICATE_TYPES,
        )

        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            struct0:
                description: foo
            struct1:
                description: foo
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - array<struct0>
                            - array<struct1>
            """),
            idl.errors.ERROR_ID_VARIANT_DUPLICATE_TYPES,
        )

        # At most one array can have BSON serialization type NumberInt.
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - array<int>
                            - array<safeInt>
            """),
            idl.errors.ERROR_ID_VARIANT_DUPLICATE_TYPES,
        )

        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            one_string:
                description: foo
                fields: {value: string}
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - one_string
                            - one_string
                            - int
            """),
            idl.errors.ERROR_ID_VARIANT_STRUCTS,
            True,
        )

        # For multiple BSON serialization type Objects they must have different field names
        # for their first field.
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            one_string:
                description: foo
                fields: {value: string}
            one_int:
                description: foo
                fields: {value: int}
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - one_string
                            - one_int
                            - int
            """),
            idl.errors.ERROR_ID_VARIANT_STRUCTS,
            True,
        )

        # At most one type can have BSON serialization type NumberInt.
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - safeInt
                            - int
      """),
            idl.errors.ERROR_ID_VARIANT_DUPLICATE_TYPES,
        )

    def test_field_positive(self):
        # type: () -> None
        """Positive test cases for field."""

        # Setup some common types
        test_preamble = self.common_types

        # Short type
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        structs:
            bar:
                description: foo
                strict: false
                fields:
                    foo: string
            """)
        )

        # Long type
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        structs:
            bar:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
            """)
        )

        # Long type with default
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        structs:
            bar:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
                        default: bar
            """)
        )

        # Test array as field type
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: array<string>
            """)
        )

        # Test array as field type
        self.assert_bind(
            self.common_types
            + indent_text(
                1,
                textwrap.dedent("""
                arrayfake:
                    description: foo
                    cpp_type: foo
                    bson_serialization_type: string
                    serializer: foo
                    deserializer: foo
                    is_view: false
            """),
            )
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        arrayOfString: arrayfake
            """)
        )

        # Test always_serialize with optional
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo:
                            type: string
                            optional: true
                            always_serialize: true
            """)
        )

        # Test field of a struct type with default=true
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    fields:
                        field1: string

                bar:
                    description: foo
                    fields:
                        field2:
                            type: foo
                            default: true

            """)
        )

    def test_field_negative(self):
        # type: () -> None
        """Negative field tests."""

        # Setup some common types
        test_preamble = textwrap.dedent("""
        types:
            string:
                description: foo
                cpp_type: foo
                bson_serialization_type: string
                serializer: foo
                deserializer: foo
                default: foo
                is_view: false

            bindata:
                description: foo
                cpp_type: foo
                bson_serialization_type: bindata
                bindata_subtype: uuid
                is_view: false

            serialization_context:
                bson_serialization_type: any
                description: foo
                cpp_type: foo
                internal_only: true
                is_view: false
        """)

        # Test field of a struct type with a non-true default
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    fields:
                        field1: string

                bar:
                    description: foo
                    fields:
                        field2:
                            type: foo
                            default: foo

            """),
            idl.errors.ERROR_ID_DEFAULT_MUST_BE_TRUE_OR_EMPTY_FOR_STRUCT,
        )

        # Test array as field name
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        array<foo>: string
            """),
            idl.errors.ERROR_ID_ARRAY_NOT_VALID_TYPE,
        )

        # Test recursive array as field type
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: array<array<string>>
            """),
            idl.errors.ERROR_ID_BAD_ARRAY_TYPE_NAME,
            True,
        )

        # Test inherited default with array
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo: array<string>
            """),
            idl.errors.ERROR_ID_ARRAY_NO_DEFAULT,
        )

        # Test non-inherited default with array
        self.assert_bind_fail(
            self.common_types
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo:
                            type: array<string>
                            default: 123
            """),
            idl.errors.ERROR_ID_ARRAY_NO_DEFAULT,
        )

        # Test bindata with default
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo:
                            type: bindata
                            default: 42
            """),
            idl.errors.ERROR_ID_BAD_BINDATA_DEFAULT,
        )

        # Test default and optional for the same field
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo:
                            type: string
                            default: 42
                            optional: true
            """),
            idl.errors.ERROR_ID_ILLEGAL_FIELD_DEFAULT_AND_OPTIONAL,
        )

        # Test always_serialize without optional for the same field
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    strict: true
                    fields:
                        foo:
                            type: string
                            default: 42
                            always_serialize: true
            """),
            idl.errors.ERROR_ID_ILLEGAL_FIELD_ALWAYS_SERIALIZE_NOT_OPTIONAL,
        )

        # Test duplicate comparison order
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                generate_comparison_operators: true
                fields:
                    foo:
                        type: string
                        comparison_order: 1
                    bar:
                        type: string
                        comparison_order: 1
            """),
            idl.errors.ERROR_ID_IS_DUPLICATE_COMPARISON_ORDER,
        )

        # Test field marked with non_const_getter in immutable struct
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    immutable: true
                    fields:
                        foo:
                            type: string
                            non_const_getter: true
            """),
            idl.errors.ERROR_ID_NON_CONST_GETTER_IN_IMMUTABLE_STRUCT,
        )

    def test_ignored_field_negative(self):
        # type: () -> None
        """Test that if a field is marked as ignored, no other properties are set."""
        for test_value in [
            "optional: true",
            "default: foo",
        ]:
            self.assert_bind_fail(
                self.common_types
                + textwrap.dedent(
                    """
            structs:
                foo:
                    description: foo
                    strict: false
                    fields:
                        foo:
                            type: string
                            ignore: true
                            %s
                """
                    % (test_value)
                ),
                idl.errors.ERROR_ID_FIELD_MUST_BE_EMPTY_FOR_IGNORED,
            )

    def test_chained_struct_positive(self):
        # type: () -> None
        """Positive parser chaining test cases."""
        # Setup some common types
        test_preamble = (
            self.common_types
            + indent_text(
                1,
                textwrap.dedent("""
            foo1:
                description: foo
                cpp_type: foo
                bson_serialization_type: chain
                serializer: foo
                deserializer: foo
                default: foo
                is_view: false
        """),
            )
            + textwrap.dedent("""
        structs:
            chained:
                description: foo
                strict: false

            chained2:
                description: foo
                strict: false
                fields:
                    field1: string
        """)
        )

        # A struct with only chaining
        self.assert_bind(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                chained_structs:
                    chained2: alias
        """),
            )
        )

        # Chaining struct's fields and explicit fields
        self.assert_bind(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                chained_structs:
                    chained2: alias
                fields:
                    str1: string
        """),
            )
        )

        # Chained types and structs
        self.assert_bind(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_structs:
                    chained2: alias
                fields:
                    str1: string
        """),
            )
        )

        # Non-strict chained struct
        self.assert_bind(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_structs:
                    chained2: alias
                fields:
                    foo1: string
        """),
            )
        )

        # Inline Chained struct with strict true
        self.assert_bind(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                fields:
                    field1: string

            foobar:
                description: foo
                strict: false
                inline_chained_structs: true
                chained_structs:
                    bar1: alias
                fields:
                    f1: string

        """),
            )
        )

        # Inline Chained struct with strict true and inline_chained_structs defaulted
        self.assert_bind(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                fields:
                    field1: string

            foobar:
                description: foo
                strict: false
                chained_structs:
                    bar1: alias
                fields:
                    f1: string
        """),
            )
        )

        # Chained struct with nested chained struct
        self.assert_bind(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_structs:
                    chained: alias

            foobar:
                description: foo
                strict: false
                chained_structs:
                    bar1: alias
                fields:
                    f1: string

        """),
            )
        )

    def test_chained_struct_negative(self):
        # type: () -> None
        """Negative parser chaining test cases."""
        # Setup some common types
        test_preamble = (
            self.common_types
            + indent_text(
                1,
                textwrap.dedent("""
            foo1:
                description: foo
                cpp_type: foo
                bson_serialization_type: chain
                serializer: foo
                deserializer: foo
                default: foo
                is_view: false
        """),
            )
            + textwrap.dedent("""
        structs:
            chained:
                description: foo
                strict: false
                fields:
                    field1: string

            chained2:
                description: foo
                strict: false
                fields:
                    field1: string
        """)
        )

        # Non-existing chained struct
        self.assert_bind_fail(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                chained_structs:
                    foobar1: alias
        """),
            ),
            idl.errors.ERROR_ID_UNKNOWN_TYPE,
            True,
        )

        # Type as chained struct
        self.assert_bind_fail(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                chained_structs:
                    foo1: alias
        """),
            ),
            idl.errors.ERROR_ID_CHAINED_STRUCT_NOT_FOUND,
        )

        # Duplicated field names across chained struct's fields and fields
        self.assert_bind_fail(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_structs:
                    chained: alias
                fields:
                    field1: string
        """),
            ),
            idl.errors.ERROR_ID_CHAINED_DUPLICATE_FIELD,
        )

        # Duplicated field names across chained structs
        self.assert_bind_fail(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: false
                chained_structs:
                    chained: alias
                    chained2: alias
        """),
            ),
            idl.errors.ERROR_ID_CHAINED_DUPLICATE_FIELD,
        )

        # Chained struct with strict true
        self.assert_bind_fail(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
            bar1:
                description: foo
                strict: true
                fields:
                    field1: string

            foobar:
                description: foo
                strict: false
                inline_chained_structs: false
                chained_structs:
                    bar1: alias
                fields:
                    f1: string

        """),
            ),
            idl.errors.ERROR_ID_CHAINED_NO_NESTED_STRUCT_STRICT,
        )

    def test_enum_positive(self):
        # type: () -> None
        """Positive enum test cases."""

        # Test int
        self.assert_bind(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: 3
                    v2: 1
                    v3: 2
            """)
        )

        # Test int - non continuous
        self.assert_bind(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: 0
                    v3: 2
            """)
        )

        # Test string
        self.assert_bind(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: string
                values:
                    v1: 0
                    v2: 1
                    v3: 2
            """)
        )

    def test_enum_negative(self):
        # type: () -> None
        """Negative enum test cases."""

        # Test wrong type
        self.assert_bind_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: foo
                values:
                    v1: 0
            """),
            idl.errors.ERROR_ID_ENUM_BAD_TYPE,
        )

        # Test int - dups
        self.assert_bind_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: 1
                    v3: 1
            """),
            idl.errors.ERROR_ID_ENUM_NON_UNIQUE_VALUES,
        )

        # Test int - non-integer value
        self.assert_bind_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: foo
                    v3: 1
            """),
            idl.errors.ERROR_ID_ENUM_BAD_INT_VAUE,
        )

        # Test string - dups
        self.assert_bind_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: string
                values:
                    v1: foo
                    v3: foo
            """),
            idl.errors.ERROR_ID_ENUM_NON_UNIQUE_VALUES,
        )

    def test_struct_enum_negative(self):
        # type: () -> None
        """Negative enum test cases."""

        test_preamble = self.common_types + textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: 0
                    v2: 1
        """)

        # Test array of enums
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        structs:
            foo1:
                description: foo
                fields:
                    foo1: array<foo>
            """),
            idl.errors.ERROR_ID_NO_ARRAY_ENUM,
            True,
        )

    def test_command_positive(self):
        # type: () -> None
        """Positive command tests."""

        # Setup some common types
        test_preamble = self.common_types + textwrap.dedent("""
        structs:
            reply:
                description: foo
                fields:
                    foo: string
        """)

        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    strict: true
                    fields:
                        foo1: string
                    reply_type: reply
            """)
        )

    def test_command_negative(self):
        # type: () -> None
        """Negative command tests."""

        # Setup some common types
        test_preamble = self.common_types
        # Commands cannot be fields in other commands
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    fields:
                        foo1: string

                bar:
                    description: foo
                    command_name: bar
                    namespace: ignored
                    api_version: ""
                    fields:
                        foo: foo
            """),
            idl.errors.ERROR_ID_FIELD_NO_COMMAND,
        )

        # Commands cannot be fields in structs
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    fields:
                        foo1: string

            structs:
                bar:
                    description: foo
                    fields:
                        foo: foo
            """),
            idl.errors.ERROR_ID_FIELD_NO_COMMAND,
        )

        # Commands cannot have a field as the same name
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    fields:
                        foo: string
            """),
            idl.errors.ERROR_ID_COMMAND_DUPLICATES_FIELD,
        )

        # Reply type must be resolvable
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    reply_type: not_defined
            """),
            idl.errors.ERROR_ID_UNKNOWN_TYPE,
        )

        # Reply type must be a struct
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    reply_type: string
            """),
            idl.errors.ERROR_ID_INVALID_REPLY_TYPE,
        )

    def test_command_doc_sequence_positive(self):
        # type: () -> None
        """Positive supports_doc_sequence tests."""

        # Setup some common types
        test_preamble = self.common_types + textwrap.dedent("""
        structs:
            foo_struct:
                description: foo
                strict: true
                fields:
                    foo: object
        """)

        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    fields:
                        foo1:
                            type: array<object>
                            supports_doc_sequence: true
            """)
        )

        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    fields:
                        foo1:
                            type: array<foo_struct>
                            supports_doc_sequence: true
            """)
        )

    def test_command_doc_sequence_negative(self):
        # type: () -> None
        """Negative supports_doc_sequence tests."""

        # Setup some common types
        test_preamble = self.common_types

        test_preamble2 = test_preamble + textwrap.dedent("""
        structs:
            foo_struct:
                description: foo
                strict: true
                fields:
                    foo: object
        """)

        # A struct
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            structs:
                foo:
                    description: foo
                    fields:
                        foo:
                            type: array<object>
                            supports_doc_sequence: true
            """),
            idl.errors.ERROR_ID_STRUCT_NO_DOC_SEQUENCE,
        )

        # A non-array type
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    fields:
                        foo:
                            type: object
                            supports_doc_sequence: true
            """),
            idl.errors.ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_ARRAY,
        )

        # An array of a scalar
        self.assert_bind_fail(
            test_preamble2
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    fields:
                        foo1:
                            type: array<string>
                            supports_doc_sequence: true
            """),
            idl.errors.ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_OBJECT,
        )

        # An array of 'any'
        self.assert_bind_fail(
            test_preamble2
            + textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: ""
                    fields:
                        foo1:
                            type: array<string>
                            supports_doc_sequence: true
            """),
            idl.errors.ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_OBJECT,
        )

    def test_command_type_positive(self):
        # type: () -> None
        """Positive command custom type test cases."""
        # Setup some common types
        test_preamble = self.common_types

        # string
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                strict: true
                namespace: type
                api_version: ""
                type: string
                fields:
                    field1: string
            """)
        )

        # array of string
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                strict: true
                namespace: type
                api_version: ""
                type: array<string>
                fields:
                    field1: string
            """)
        )

    def test_command_type_negative(self):
        # type: () -> None
        """Negative command type test cases."""
        # Setup some common types
        test_preamble = self.common_types

        # supports_doc_sequence must be a bool
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: type
                api_version: ""
                type: int
                fields:
                    field1: string
            """),
            idl.errors.ERROR_ID_UNKNOWN_TYPE,
            True,
        )

    def test_server_parameter_positive(self):
        # type: () -> None
        """Positive server parameter test cases."""

        # server parameter with storage.
        # Also try valid set_at values.
        for set_at in ["startup", "runtime", "[ startup, runtime ]", "cluster"]:
            if set_at != "cluster":
                self.assert_bind(
                    textwrap.dedent(
                        """
                server_parameters:
                    foo:
                        set_at: %s
                        description: bar
                        redact: false
                        cpp_varname: baz
                    """
                        % (set_at)
                    )
                )
            else:
                self.assert_bind(
                    textwrap.dedent(
                        """
                server_parameters:
                    foo:
                        set_at: %s
                        description: bar
                        redact: false
                        cpp_varname: baz
                        omit_in_ftdc: false
                    """
                        % (set_at)
                    )
                )

        # server parameter with storage and optional fields.
        self.assert_bind(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: startup
                description: bar
                redact: false
                cpp_varname: baz
                default: 42
                on_update: buzz
                validator:
                    gt: 0
                    gte: 1
                    lte: 999
                    lt: 1000
                    callback: qux
            """)
        )

        # Cluster server parameter with storage.
        self.assert_bind(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: cluster
                description: bar
                redact: false
                cpp_varname: baz
                cpp_vartype: bazStorage
                on_update: buzz
                omit_in_ftdc: false
                validator:
                    gt: 0
                    gte: 1
                    lte: 999
                    lt: 1000
                    callback: qux
            """)
        )

        # Bound setting with arbitrary expression default and validators.
        self.assert_bind(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: startup
                description: bar
                redact: false
                cpp_varname: baz
                default:
                    expr: 'kDefaultValue'
                validator:
                    gte:
                        expr: 'kMinimumValue'
                        is_constexpr: true
                    lte:
                        expr: 'kMaximumValue'
                        is_constexpr: false
                    gt: 0
                    lt: 255
            """)
        )

        # Specialized SCPs.
        self.assert_bind(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: startup
                description: bar
                redact: false
                cpp_class: baz
        """)
        )

        self.assert_bind(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: startup
                description: bar
                redact: false
                cpp_class:
                    name: baz
        """)
        )

        self.assert_bind(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: startup
                description: bar
                redact: false
                cpp_class:
                    name: baz
                    data: bling
                    override_set: true
                    override_ctor: false
                    override_validate: true
        """)
        )

        self.assert_bind(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: startup
                description: bar
                cpp_class: baz
                condition: { expr: "true" }
                redact: true
                test_only: true
                deprecated_name: bling
        """)
        )

        self.assert_bind(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: cluster
                description: bar
                cpp_class:
                    name: baz
                    override_validate: true
                condition: { expr: "true" }
                redact: true
                omit_in_ftdc: true
                test_only: true
                deprecated_name: bling
        """)
        )

        # Default without data.
        self.assert_bind(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: startup
                description: bar
                redact: false
                cpp_class: baz
                default: blong
            """)
        )

    def test_server_parameter_negative(self):
        # type: () -> None
        """Negative server parameter test cases."""

        # Invalid set_at values.
        self.assert_bind_fail(
            textwrap.dedent("""
            server_parameters:
                foo:
                    set_at: shutdown
                    description: bar
                    redact: false
                    cpp_varname: baz
            """),
            idl.errors.ERROR_ID_BAD_SETAT_SPECIFIER,
        )

        # Mix of specialized with bound storage.
        self.assert_bind_fail(
            textwrap.dedent("""
            server_parameters:
                foo:
                    set_at: startup
                    description: bar
                    redact: false
                    cpp_class: baz
                    cpp_varname: bling
            """),
            idl.errors.ERROR_ID_SERVER_PARAMETER_INVALID_ATTR,
        )

        # Startup with omit_in_ftdc=true.
        self.assert_bind_fail(
            textwrap.dedent("""
            server_parameters:
                foo:
                    set_at: startup
                    description: bar
                    cpp_varname: baz
                    redact: false
                    omit_in_ftdc: true
            """),
            idl.errors.ERROR_ID_SERVER_PARAMETER_INVALID_ATTR,
        )

        # Startup with omit_in_ftdc=false.
        self.assert_bind_fail(
            textwrap.dedent("""
            server_parameters:
                foo:
                    set_at: startup
                    description: bar
                    cpp_varname: baz
                    redact: false
                    omit_in_ftdc: false
            """),
            idl.errors.ERROR_ID_SERVER_PARAMETER_INVALID_ATTR,
        )

        # Runtime with omit_in_ftdc=true.
        self.assert_bind_fail(
            textwrap.dedent("""
            server_parameters:
                foo:
                    set_at: runtime
                    description: bar
                    cpp_varname: baz
                    redact: false
                    omit_in_ftdc: true
            """),
            idl.errors.ERROR_ID_SERVER_PARAMETER_INVALID_ATTR,
        )

        # Runtime with omit_in_ftdc=false.
        self.assert_bind_fail(
            textwrap.dedent("""
            server_parameters:
                foo:
                    set_at: runtime
                    description: bar
                    cpp_varname: baz
                    redact: false
                    omit_in_ftdc: false
            """),
            idl.errors.ERROR_ID_SERVER_PARAMETER_INVALID_ATTR,
        )

        # Cluster with omit_in_ftdc unspecified.
        self.assert_bind_fail(
            textwrap.dedent("""
            server_parameters:
                foo:
                    set_at: cluster
                    description: bar
                    redact: false
                    cpp_varname: baz
                    cpp_vartype: bazStorage
                    on_update: buzz
                    validator:
                        gt: 0
                        gte: 1
                        lte: 999
                        lt: 1000
                        callback: qux
                """),
            idl.errors.ERROR_ID_SERVER_PARAMETER_REQUIRED_ATTR,
        )

    def test_config_option_positive(self):
        # type: () -> None
        """Positive config option test cases."""

        # Every field.
        self.assert_bind(
            textwrap.dedent("""
            configs:
                foo:
                    short_name: bar
                    deprecated_name: baz
                    deprecated_short_name: qux
                    description: comment
                    section: here
                    arg_vartype: String
                    cpp_varname: gStringVal
                    conflicts: bling
                    requires: blong
                    hidden: false
                    default: one
                    implicit: two
                    duplicate_behavior: append
                    source: yaml
                    positional: 1-2
                    validator:
                        gt: 0
                        lt: 100
                        gte: 1
                        lte: 99
                        callback: doSomething
            """)
        )

        # Required fields only.
        self.assert_bind(
            textwrap.dedent("""
            configs:
                foo:
                    description: comment
                    arg_vartype: Switch
                    source: yaml
            """)
        )

        # List and enum variants.
        self.assert_bind(
            textwrap.dedent("""
            configs:
                foo:
                    deprecated_name: [ baz, baz ]
                    deprecated_short_name: [ bling, blong ]
                    description: comment
                    arg_vartype: StringVector
                    source: [ cli, ini, yaml ]
                    conflicts: [ a, b, c ]
                    requires: [ d, e, f ]
                    hidden: true
                    duplicate_behavior: overwrite
            """)
        )

        # Positional variants.
        for positional in ["1", "1-", "-2", "1-2"]:
            self.assert_bind(
                textwrap.dedent(
                    """
                configs:
                    foo:
                        short_name: foo
                        description: comment
                        arg_vartype: Bool
                        source: cli
                        positional: %s
                """
                    % (positional)
                )
            )
            # With implicit short name.
            self.assert_bind(
                textwrap.dedent(
                    """
                configs:
                    foo:
                        description: comment
                        arg_vartype: Bool
                        source: cli
                        positional: %s
                """
                    % (positional)
                )
            )

        # Expressions in default, implicit, and validators.
        self.assert_bind(
            textwrap.dedent("""
            configs:
                foo:
                    description: bar
                    arg_vartype: String
                    source: cli
                    default: { expr: kDefault, is_constexpr: true }
                    implicit: { expr: kImplicit, is_constexpr: false }
                    validator:
                        gte: { expr: kMinimum }
                        lte: { expr: kMaximum }
            """)
        )

    def test_config_option_negative(self):
        # type: () -> None
        """Negative config option test cases."""

        # Invalid source.
        self.assert_bind_fail(
            textwrap.dedent("""
            configs:
                foo:
                    description: comment
                    arg_vartype: Long
                    source: json
            """),
            idl.errors.ERROR_ID_BAD_SOURCE_SPECIFIER,
        )

        self.assert_bind_fail(
            textwrap.dedent("""
            configs:
                foo:
                    description: comment
                    arg_vartype: StringMap
                    source: [ cli, yaml ]
                    duplicate_behavior: guess
            """),
            idl.errors.ERROR_ID_BAD_DUPLICATE_BEHAVIOR_SPECIFIER,
        )

        for positional in ["x", "1-2-3", "-2-", "1--3"]:
            self.assert_bind_fail(
                textwrap.dedent(
                    """
                configs:
                    foo:
                        description: comment
                        arg_vartype: String
                        source: cli
                        positional: %s
                """
                    % (positional)
                ),
                idl.errors.ERROR_ID_BAD_NUMERIC_RANGE,
            )

        self.assert_bind_fail(
            textwrap.dedent("""
            configs:
                foo:
                    description: comment
                    short_name: "bar.baz"
                    arg_vartype: Bool
                    source: cli
            """),
            idl.errors.ERROR_ID_INVALID_SHORT_NAME,
        )

        self.assert_bind_fail(
            textwrap.dedent("""
            configs:
                foo:
                    description: comment
                    short_name: bar
                    deprecated_short_name: "baz.qux"
                    arg_vartype: Long
                    source: cli
            """),
            idl.errors.ERROR_ID_INVALID_SHORT_NAME,
        )

        # dottedName is not valid as a shortName.
        self.assert_bind_fail(
            textwrap.dedent("""
            configs:
                "foo.bar":
                    description: comment
                    arg_vartype: String
                    source: cli
                    positional: 1
            """),
            idl.errors.ERROR_ID_MISSING_SHORTNAME_FOR_POSITIONAL,
        )

        # Invalid shortname using boost::po format directly.
        self.assert_bind_fail(
            textwrap.dedent("""
            configs:
                foo:
                    short_name: "foo,f"
                    arg_vartype: Switch
                    description: comment
                    source: cli
            """),
            idl.errors.ERROR_ID_INVALID_SHORT_NAME,
        )

        # Invalid single names, must be single alpha char.
        for name in ["foo", "1", ".", ""]:
            self.assert_bind_fail(
                textwrap.dedent(
                    """
                configs:
                    foo:
                        single_name: "%s"
                        arg_vartype: Switch
                        description: comment
                        source: cli
            """
                    % (name)
                ),
                idl.errors.ERROR_ID_INVALID_SINGLE_NAME,
            )

        # Single names require a valid short name.
        self.assert_bind_fail(
            textwrap.dedent("""
            configs:
                "foo.bar":
                    single_name: f
                    arg_vartype: Switch
                    description: comment
                    source: cli
            """),
            idl.errors.ERROR_ID_MISSING_SHORT_NAME_WITH_SINGLE_NAME,
        )

    def test_feature_flag(self):
        # type: () -> None
        """Test feature flag checks around version."""

        # feature flag can default to false without a version (fcv_gated can be true or false)
        self.assert_bind(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: false
                    fcv_gated: false
            """)
        )

        self.assert_bind(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: false
                    fcv_gated: true
            """)
        )

        # if fcv_gated: true, feature flag can default to true with a version
        self.assert_bind(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: true
                    version: 123
                    fcv_gated: true
            """)
        )

        # if fcv_gated: false, we do not need a version
        self.assert_bind(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: true
                    fcv_gated: false
            """)
        )

        # IFR flag does not need a default or version
        self.assert_bind(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: released
                    fcv_gated: false
            """)
        )

        # IFR flag can specify a compatible version
        self.assert_bind(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: released
                    default: true
                    fcv_gated: false
            """)
        )

        # explicitly mark non-IFR flag
        self.assert_bind(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: not_for_incremental_rollout
                    default: true
                    version: 123
                    fcv_gated: true
            """)
        )

        # if fcv_gated: true and default: true, a version is required
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: true
                    fcv_gated: true
            """),
            idl.errors.ERROR_ID_FEATURE_FLAG_DEFAULT_TRUE_MISSING_VERSION,
        )

        # false is not allowed with a version and fcv_gated: true
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: false
                    version: 123
                    fcv_gated: true
            """),
            idl.errors.ERROR_ID_FEATURE_FLAG_DEFAULT_FALSE_HAS_VERSION,
        )

        # false is not allowed with a version and fcv_gated: false
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: false
                    version: 123
                    fcv_gated: false
            """),
            idl.errors.ERROR_ID_FEATURE_FLAG_DEFAULT_FALSE_HAS_VERSION,
        )

        # if fcv_gated is false, a version is not allowed
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: true
                    version: 123
                    fcv_gated: false
            """),
            idl.errors.ERROR_ID_FEATURE_FLAG_SHOULD_BE_FCV_GATED_FALSE_HAS_UNSUPPORTED_OPTION,
        )

        # if fcv_gated is false, enable_on_transitional_fcv_UNSAFE is not allowed
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: >
                      Make toast
                      (Enable on transitional FCV): Lorem ipsum dolor sit amet
                    cpp_varname: gToaster
                    default: true
                    fcv_gated: false
                    enable_on_transitional_fcv_UNSAFE: true
            """),
            idl.errors.ERROR_ID_FEATURE_FLAG_SHOULD_BE_FCV_GATED_FALSE_HAS_UNSUPPORTED_OPTION,
        )

        # if fcv_gated: true, enable_on_transitional_fcv_UNSAFE is allowed
        self.assert_bind(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: >
                      Make toast
                      (Enable on transitional FCV): Lorem ipsum dolor sit amet
                    cpp_varname: gToaster
                    default: true
                    version: 123
                    fcv_gated: true
                    enable_on_transitional_fcv_UNSAFE: true
            """)
        )

        # if enable_on_transitional_fcv_UNSAFE: true, the description must contain a
        # "(Enable on transitional FCV): ..." text explaining why the usage is safe
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: true
                    version: 123
                    fcv_gated: true
                    enable_on_transitional_fcv_UNSAFE: true
            """),
            idl.errors.ERROR_ID_FEATURE_FLAG_ENABLED_ON_TRANSITIONAL_FCV_MISSING_SAFETY_EXPLANATION,
        )

        # if fcv_gated is false, fcv_context_unaware is not allowed
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: true
                    fcv_gated: false
                    fcv_context_unaware: true
            """),
            idl.errors.ERROR_ID_FEATURE_FLAG_SHOULD_BE_FCV_GATED_FALSE_HAS_UNSUPPORTED_OPTION,
        )

        # if fcv_gated: true, fcv_context_unaware is allowed
        self.assert_bind(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: false
                    fcv_gated: true
                    fcv_context_unaware: true
            """)
        )

        # incremental_rollout_phase must have a valid value
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: waning_gibbous
                    fcv_gated: false
            """),
            idl.errors.ERROR_ID_INCREMENTAL_ROLLOUT_PHASE_INVALID_VALUE,
        )

        # if set for incremental feature rollout (IFR), version not allowed
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: released
                    default: true
                    fcv_gated: true
            """),
            idl.errors.ERROR_ID_ILLEGALLY_FCV_GATED_FEATURE_FLAG,
        )

        # incremental_rollout_phase must not specify incompatible default
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: in_development
                    default: true
                    fcv_gated: false
            """),
            idl.errors.ERROR_ID_INCREMENTAL_FEATURE_FLAG_DEFAULT_VALUE,
        )

        # default required for non-IFR feature flag
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    fcv_gated: true
            """),
            idl.errors.ERROR_ID_FEATURE_FLAG_WITHOUT_DEFAULT_VALUE,
        )
        # incremental_rollout_phase must have a valid value
        self.assert_bind_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: rollout
                    version: 123
                    fcv_gated: false
            """),
            idl.errors.ERROR_ID_IFR_FLAG_WITH_VERSION,
        )

    def test_access_check(self):
        # type: () -> None
        """Test access check."""

        test_preamble = self.common_types + textwrap.dedent("""
        enums:
            AccessCheck:
                description: "test"
                type: string
                values:
                    kIsAuthenticated :  "is_authenticated"
                    kIsCoAuthorized :  "is_coauthorized"

            ActionType:
                description: "test"
                type: string
                values:
                    addShard :  "addShard"
                    serverStatus :  "serverStatus"

            MatchType:
                description: "test"
                type: string
                values:
                    matchClusterResource :  "cluster"

        structs:
            reply:
                description: foo
                fields:
                    foo: string
        """)

        # Test none
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: ""
                namespace: ignored
                access_check:
                    none: true
                fields:
                    foo: string
                reply_type: reply
            """)
        )

        # Test simple with access check
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: ""
                namespace: ignored
                access_check:
                    simple:
                        check: is_authenticated
                fields:
                    foo: string
                reply_type: reply
            """)
        )

        # Test simple with privilege
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: ""
                namespace: ignored
                access_check:
                    simple:
                        privilege:
                            resource_pattern: cluster
                            action_type: addShard
                fields:
                    foo: string
                reply_type: reply
            """)
        )

        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    complex:
                        - privilege:
                            resource_pattern: cluster
                            action_type: addShard
                        - privilege:
                            resource_pattern: cluster
                            action_type: serverStatus
                        - check: is_authenticated
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """)
        )

    def test_access_check_negative(self):
        # type: () -> None
        """Negative access check tests."""

        test_preamble = self.common_types + textwrap.dedent("""
        enums:
            AccessCheck:
                description: "test"
                type: string
                values:
                    kIsAuthenticated :  "is_authenticated"
                    kIsCoAuthorized :  "is_coauthorized"

            ActionType:
                description: "test"
                type: string
                values:
                    addShard :  "addShard"
                    serverStatus :  "serverStatus"

            MatchType:
                description: "test"
                type: string
                values:
                    matchClusterResource :  "cluster"
        structs:
            reply:
                description: foo
                fields:
                    foo: string
        """)

        # Test simple with bad access check
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: ""
                namespace: ignored
                access_check:
                    simple:
                        check: unknown
                fields:
                    foo: string
                reply_type: reply
            """),
            idl.errors.ERROR_ID_UNKOWN_ENUM_VALUE,
        )

        # Test simple with bad access check with privilege
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: ""
                namespace: ignored
                access_check:
                    simple:
                        privilege:
                            resource_pattern: foo
                            action_type: addShard
                fields:
                    foo: string
                reply_type: reply
            """),
            idl.errors.ERROR_ID_UNKOWN_ENUM_VALUE,
        )

        # Test simple with bad access check with privilege
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: ""
                namespace: ignored
                access_check:
                    simple:
                        privilege:
                            resource_pattern: cluster
                            action_type: foo
                fields:
                    foo: string
                reply_type: reply
            """),
            idl.errors.ERROR_ID_UNKOWN_ENUM_VALUE,
        )

        # Test simple with access check and privileges
        self.assert_bind(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: ""
                namespace: ignored
                access_check:
                    simple:
                        privilege:
                            resource_pattern: cluster
                            action_type: [addShard, serverStatus]
                fields:
                    foo: string
                reply_type: reply
            """)
        )

        # Test simple with privilege with duplicate action_type
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: ""
                namespace: ignored
                access_check:
                    simple:
                        privilege:
                            resource_pattern: cluster
                            action_type: [addShard, addShard]
                fields:
                    foo: string
                reply_type: reply
            """),
            idl.errors.ERROR_ID_DUPLICATE_ACTION_TYPE,
        )

        # complex with duplicate check
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    complex:
                        - check: is_authenticated
                        - check: is_authenticated
                fields:
                    foo: string
                reply_type: reply
            """),
            idl.errors.ERROR_ID_DUPLICATE_ACCESS_CHECK,
        )

        # complex with duplicate priv
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    complex:
                        - privilege:
                            resource_pattern: cluster
                            action_type: addShard
                        - privilege:
                            resource_pattern: cluster
                            action_type: [addShard, serverStatus]
                fields:
                    foo: string
                reply_type: reply
            """),
            idl.errors.ERROR_ID_DUPLICATE_ACCESS_CHECK,
        )

        # api_version != "" but not access_check
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
        commands:
            test1:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                fields:
                    foo: string
                reply_type: reply
            """),
            idl.errors.ERROR_ID_MISSING_ACCESS_CHECK,
        )

    def test_query_shape_component_validation(self):
        self.assert_bind(
            self.common_types
            + textwrap.dedent("""
            structs:
                struct1:
                    query_shape_component: true
                    strict: true
                    description: ""
                    fields:
                        field1:
                            query_shape: literal
                            type: string
                        field2:
                            type: bool
                            query_shape: parameter
        """)
        )

        self.assert_bind_fail(
            self.common_types
            + textwrap.dedent("""
            structs:
                struct1:
                    query_shape_component: true
                    strict: true
                    description: ""
                    fields:
                        field1:
                            type: string
                        field2:
                            type: bool
                            query_shape: parameter
        """),
            idl.errors.ERROR_ID_FIELD_MUST_DECLARE_SHAPE_LITERAL,
        )

        self.assert_bind_fail(
            self.common_types
            + textwrap.dedent("""
            structs:
                struct1:
                    strict: true
                    description: ""
                    fields:
                        field1:
                            type: string
                        field2:
                            type: bool
                            query_shape: parameter
        """),
            idl.errors.ERROR_ID_CANNOT_DECLARE_SHAPE_LITERAL,
        )

        # Validating query_shape_anonymize relies on std::string
        basic_types = textwrap.dedent("""
            types:
                string:
                    bson_serialization_type: string
                    description: "A BSON UTF-8 string"
                    cpp_type: "std::string"
                    deserializer: "mongo::BSONElement::str"
                    is_view: false
                bool:
                    bson_serialization_type: bool
                    description: "A BSON bool"
                    cpp_type: "bool"
                    deserializer: "mongo::BSONElement::boolean"
                    is_view: false
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false
        """)
        self.assert_bind(
            basic_types
            + textwrap.dedent("""
            structs:
                struct1:
                    query_shape_component: true
                    strict: true
                    description: ""
                    fields:
                        field1:
                            query_shape: anonymize
                            type: string
                        field2:
                            query_shape: parameter
                            type: bool
        """)
        )

        self.assert_bind(
            basic_types
            + textwrap.dedent("""
            structs:
                struct1:
                    query_shape_component: true
                    strict: true
                    description: ""
                    fields:
                        field1:
                            query_shape: anonymize
                            type: array<string>
                        field2:
                            query_shape: parameter
                            type: bool
        """)
        )

        self.assert_bind_fail(
            basic_types
            + textwrap.dedent("""
            structs:
                struct1:
                    strict: true
                    description: ""
                    fields:
                        field1:
                            query_shape: blah
                            type: string
        """),
            idl.errors.ERROR_ID_QUERY_SHAPE_INVALID_VALUE,
        )

        self.assert_bind_fail(
            basic_types
            + textwrap.dedent("""
            structs:
                struct1:
                    query_shape_component: true
                    strict: true
                    description: ""
                    fields:
                        field1:
                            query_shape: anonymize
                            type: bool
                        field2:
                            query_shape: parameter
                            type: bool
        """),
            idl.errors.ERROR_ID_INVALID_TYPE_FOR_SHAPIFY,
        )

        self.assert_bind_fail(
            basic_types
            + textwrap.dedent("""
            structs:
                struct1:
                    query_shape_component: true
                    strict: true
                    description: ""
                    fields:
                        field1:
                            query_shape: anonymize
                            type: array<bool>
                        field2:
                            query_shape: parameter
                            type: bool
        """),
            idl.errors.ERROR_ID_INVALID_TYPE_FOR_SHAPIFY,
        )

        self.assert_bind_fail(
            basic_types
            + textwrap.dedent("""
            structs:
                StructZero:
                    strict: true
                    description: ""
                    fields:
                        field1:
                            query_shape: literal
                            type: string
            """),
            idl.errors.ERROR_ID_CANNOT_DECLARE_SHAPE_LITERAL,
        )

        self.assert_bind_fail(
            basic_types
            + textwrap.dedent("""
            structs:
                StructZero:
                    strict: true
                    description: ""
                    fields:
                        field1:
                            type: string
                struct1:
                    query_shape_component: true
                    strict: true
                    description: ""
                    fields:
                        field2:
                            type: StructZero
                            description: ""
                            query_shape: literal
            """),
            idl.errors.ERROR_ID_CANNOT_DECLARE_SHAPE_LITERAL,
        )

    def test_struct_unsafe_dangerous_disable_extra_field_duplicate_checks_negative(self):
        # type: () -> None
        """Negative struct tests for unsafe_dangerous_disable_extra_field_duplicate_checks."""

        # Setup some common types
        test_preamble = self.common_types + textwrap.dedent("""
            structs:
                danger:
                    description: foo
                    strict: false
                    unsafe_dangerous_disable_extra_field_duplicate_checks: true
                    fields:
                        foo: string
        """)

        # Test strict and unsafe_dangerous_disable_extra_field_duplicate_checks are not allowed
        self.assert_bind_fail(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
                danger1:
                    description: foo
                    strict: true
                    unsafe_dangerous_disable_extra_field_duplicate_checks: true
                    fields:
                        foo: string
            """),
            ),
            idl.errors.ERROR_ID_STRICT_AND_DISABLE_CHECK_NOT_ALLOWED,
        )

        # Test inheritance is prohibited through structs
        self.assert_bind_fail(
            test_preamble
            + indent_text(
                1,
                textwrap.dedent("""
                danger2:
                    description: foo
                    strict: true
                    fields:
                        foo: string
                        d1: danger
            """),
            ),
            idl.errors.ERROR_ID_INHERITANCE_AND_DISABLE_CHECK_NOT_ALLOWED,
        )

        # Test inheritance is prohibited through commands
        self.assert_bind_fail(
            test_preamble
            + textwrap.dedent("""
            commands:
                dangerc:
                    description: foo
                    namespace: ignored
                    command_name: dangerc
                    strict: false
                    api_version: ""
                    fields:
                        foo: string
                        d1: danger
            """),
            idl.errors.ERROR_ID_INHERITANCE_AND_DISABLE_CHECK_NOT_ALLOWED,
        )


if __name__ == "__main__":
    unittest.main()
