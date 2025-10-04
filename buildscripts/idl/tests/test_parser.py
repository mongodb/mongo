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
"""Test cases for IDL parser."""

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
            """),
            idl.errors.ERROR_ID_UNKNOWN_ROOT,
        )

    def test_global_positive(self):
        # type: () -> None
        """Postive global tests."""
        # cpp_namespace alone
        self.assert_parse(
            textwrap.dedent("""
        global:
            cpp_namespace: 'foo'""")
        )

        # cpp_includes scalar
        self.assert_parse(
            textwrap.dedent("""
        global:
            cpp_includes: 'foo'""")
        )

        # cpp_includes list
        self.assert_parse(
            textwrap.dedent("""
        global:
            cpp_includes:
                - 'bar'
                - 'foo'""")
        )

    def test_global_negative(self):
        # type: () -> None
        """Negative global tests."""

        # Global is a scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        global: foo
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # Duplicate globals
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_namespace: 'foo'
        global:
            cpp_namespace: 'bar'
            """),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

        # Duplicate cpp_namespace
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_namespace: 'foo'
            cpp_namespace: 'foo'"""),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

        # Duplicate cpp_includes
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_includes: 'foo'
            cpp_includes: 'foo'"""),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

        # cpp_namespace as a sequence
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_namespace:
                - 'foo'
                - 'bar'"""),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # cpp_namespace as a map
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_namespace:
                name: 'foo'"""),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # cpp_includes as a map
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_includes:
                inc1: 'foo'"""),
            idl.errors.ERROR_ID_IS_NODE_TYPE_SCALAR_OR_SEQUENCE,
        )

        # cpp_includes as a sequence of tuples
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            cpp_includes:
               - inc1: 'foo'"""),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # Unknown scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        global:
            bar: 'foo'
            """),
            idl.errors.ERROR_ID_UNKNOWN_NODE,
        )

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
                is_view: false
            """)
        )

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
                is_view: false
            """)
        )

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
                is_view: false
        types:
            bar:
                description: test
                cpp_type: foo
                bson_serialization_type: int
                is_view: false
                """),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

        # Test scalar fails
        self.assert_parse_fail(
            textwrap.dedent("""
            types:
                foo: 'bar'"""),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # Test unknown field
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                bogus: foo
                description: test
                cpp_type: foo
                bson_serialization_type:
                is_view: false
                """),
            idl.errors.ERROR_ID_UNKNOWN_NODE,
        )

        # test duplicate field
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                description: test
                cpp_type: foo
                bson_serialization_type:
                is_view: false
                """),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

        # test list instead of scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            - foo:
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            multiple=True,
        )

        # test list instead of scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                - bar
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            multiple=True,
        )

        # test map instead of scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                description:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            multiple=True,
        )

        # test missing bson_serialization_type field
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                cpp_type: foo
                is_view: false
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
        )

        # test missing cpp_type field
        self.assert_parse_fail(
            textwrap.dedent("""
        types:
            foo:
                description: foo
                bson_serialization_type: foo
                is_view: false
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
        )

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
                immutable: true
                inline_chained_structs: true
                generate_comparison_operators: true
                cpp_validator_func: funcName
                fields:
                    foo: bar
            """)
        )

        # All fields with false for bools
        self.assert_parse(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                immutable: false
                inline_chained_structs: false
                generate_comparison_operators: false
                cpp_validator_func: funcName
                fields:
                    foo: bar
            """)
        )

        # Missing fields
        self.assert_parse(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: true
            """)
        )

    def test_struct_negative(self):
        # type: () -> None
        """Negative struct test cases."""

        # Struct as a scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo: foo
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # unknown field
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                foo: bar
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_UNKNOWN_NODE,
        )

        # strict is a bool
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: bar
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_BOOL,
        )

        # immutable is a bool
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                immutable: bar
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_BOOL,
        )

        # inline_chained_structs is a bool
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                inline_chained_structs: bar
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_BOOL,
        )

        # generate_comparison_operators is a bool
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                generate_comparison_operators: bar
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_BOOL,
        )

        # cpp_name is not allowed
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                cpp_name: bar
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_UNKNOWN_NODE,
        )

    def test_variant_positive(self):
        # type: () -> None
        """Positive variant test cases."""

        self.assert_parse(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field1:
                        type:
                            variant: [int, string]
                    my_variant_field2:
                        type:
                            variant:
                            - string
                            - array<string>
                            - object
            """)
        )

    def test_variant_negative(self):
        # type: () -> None
        """Negative variant test cases."""

        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant: {}
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant: 1
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant: []
                            unknown_option: true
            """),
            idl.errors.ERROR_ID_UNKNOWN_NODE,
        )

        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                fields:
                    my_variant_field:
                        type:
                            variant:
                            - string
                            - {variant: [string, int]}
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                generate_comparison_operators: true
                fields:
                    my_variant_field:
                        type:
                            variant: [string, int]
            """),
            idl.errors.ERROR_ID_VARIANT_COMPARISON,
        )

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
            """)
        )

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
                        cpp_name: bar
                        comparison_order: 3
                        stability: unstable
            """)
        )

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
                        stability: stable
            """)
        )

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
            """),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

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
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_BOOL,
        )

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
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_BOOL,
        )

        # Test bad int scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
                        comparison_order:
                            - a
                            - b
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # Test bad int
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
                        comparison_order: 3.14159
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_INT,
        )

        # Test bad negative int
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo:
                description: foo
                strict: false
                fields:
                    foo:
                        type: string
                        comparison_order: -1
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_NON_NEGATIVE_INT,
        )

    def test_chained_struct_positive(self):
        # type: () -> None
        """Positive parser chaining test cases."""
        self.assert_parse(
            textwrap.dedent("""
        structs:
            foo1:
                description: foo
                chained_structs:
                    foo1: foo1_cpp
                    foo2: foo2_cpp
        """)
        )

    def test_chained_struct_negative(self):
        # type: () -> None
        """Negative parser chaining test cases."""
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo1:
                description: foo
                chained_structs: foo1
                fields:
                    foo: bar
        """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            foo1:
                description: foo
                chained_structs:
                    - foo1
                fields:
                    foo: bar
        """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # Duplicate chained structs
        self.assert_parse_fail(
            textwrap.dedent("""
        structs:
            bar1:
                description: foo
                strict: true
                chained_structs:
                    chained: alias
                    chained: alias
        """),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

    def test_enum_positive(self):
        # type: () -> None
        """Positive enum test cases."""

        # Test all positive fields works
        self.assert_parse(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: foo
                values:
                    v1: 0
            """)
        )

        # Test extended value
        self.assert_parse(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: foo
                values:
                    v1:
                        description: foo
                        value: 0
            """)
        )

        # Test extra_data
        self.assert_parse(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: foo
                values:
                    v1:
                        description: foo
                        value: 0
                        extra_data:
                            bar: baz
            """)
        )

    def test_enum_negative(self):
        # type: () -> None
        """Negative enum test cases."""
        # Test scalar fails
        self.assert_parse_fail(
            textwrap.dedent("""
            enums:
                foo: 'bar'"""),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # Test unknown field
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                bogus: foo
                description: foo
                type: foo
                values:
                    v1: 0
                """),
            idl.errors.ERROR_ID_UNKNOWN_NODE,
        )

        # test duplicate field
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                description: test
                type: foo
                values:
                    v1: 0
                """),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

        # test list instead of scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            - foo:
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            multiple=True,
        )

        # test list instead of scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                - bar
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            multiple=True,
        )

        # test missing type field
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                values:
                    v1: 0
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
        )

        # test missing values field
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: foo
            """),
            idl.errors.ERROR_ID_BAD_EMPTY_ENUM,
        )

        # Test no values
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
            """),
            idl.errors.ERROR_ID_BAD_EMPTY_ENUM,
        )

        # Test int - duplicate names
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: 0
                    v1: 1
            """),
            idl.errors.ERROR_ID_DUPLICATE_NODE,
        )

        # Test extra_data invalid type
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1: [ 'foo' ]
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # Test extended value missing fields (description)
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1:
                        value: 0
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
        )

        # Test extended value missing fields (value)
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1:
                        description: foo
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
        )

        # Test invalid extra_data (scalar)
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1:
                        description: foo
                        value: 0
                        extra_data: foo
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # Test invalid extra_data (sequence)
        self.assert_parse_fail(
            textwrap.dedent("""
        enums:
            foo:
                description: foo
                type: int
                values:
                    v1:
                        description: foo
                        value: 0
                        extra_data: [ foo ]
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

    def test_command_positive(self):
        # type: () -> None
        """Positive command test cases."""

        # All fields with true for bools
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                strict: true
                command_name: foo
                namespace: ignored
                api_version: 1
                is_deprecated: true
                immutable: true
                inline_chained_structs: true
                generate_comparison_operators: true
                cpp_name: foo
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """)
        )

        # All fields with false for bools
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                strict: false
                namespace: ignored
                api_version: 1
                is_deprecated: false
                immutable: false
                inline_chained_structs: false
                generate_comparison_operators: false
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """)
        )

        # All fields with false for bools, empty api_version
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                strict: false
                namespace: ignored
                api_version: ""
                is_deprecated: false
                immutable: false
                inline_chained_structs: false
                generate_comparison_operators: false
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """)
        )

        # Quoted api_version
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: "1"
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """)
        )

        # Namespace ignored
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                fields:
                    foo: bar
            """)
        )

        # Namespace concatenate_with_db
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: concatenate_with_db
                api_version: ""
                fields:
                    foo: bar
            """)
        )

        # No fields
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                strict: true
            """)
        )

        # Reply type permitted without api_version
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                reply_type: foo_reply_struct
            """)
        )

    def test_command_negative(self):
        # type: () -> None
        """Negative command test cases."""

        # Command as a scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo: foo
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # unknown field
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                foo: bar
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_UNKNOWN_NODE,
        )

        # strict is a bool
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                strict: bar
                namespace: ignored
                api_version: ""
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_BOOL,
        )

        # command_name is required
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                namespace: ignored
                api_version: ""
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
        )

        # command_name is a scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: [1]
                namespace: ignored
                api_version: ""
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            True,
        )

        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: ["1"]
                namespace: ignored
                api_version: ""
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            True,
        )

        # is_deprecated is a bool
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                is_deprecated: bar
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_BOOL,
        )

        # api_version is required
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
            True,
        )

        # api_version is a scalar
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: [1]
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            True,
        )

        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ["1"]
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
            True,
        )

        # Must specify reply_type if api_version is non-empty
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: 1
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_MISSING_REPLY_TYPE,
        )

        # Namespace is required
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: ""
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
        )

        # Namespace is wrong
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: foo
                api_version: ""
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_BAD_COMMAND_NAMESPACE,
        )
        # Namespace concatenate_with_db
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: concatenate_with_db
                api_version: ""
                type: foobar
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_IS_COMMAND_TYPE_EXTRANEOUS,
        )

        # Reply type must be a scalar, not a mapping
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                reply_type:
                    arbitrary_field: foo
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

    def test_command_doc_sequence_positive(self):
        # type: () -> None
        """Positive supports_doc_sequence test cases."""

        # supports_doc_sequence can be false
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                fields:
                    foo:
                        type: bar
                        supports_doc_sequence: false
            """)
        )

        # supports_doc_sequence can be true
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                fields:
                    foo:
                        type: bar
                        supports_doc_sequence: true
            """)
        )

    def test_command_doc_sequence_negative(self):
        # type: () -> None
        """Negative supports_doc_sequence test cases."""

        # supports_doc_sequence must be a bool
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                fields:
                    foo:
                        type: bar
                        supports_doc_sequence: foo
            """),
            idl.errors.ERROR_ID_IS_NODE_VALID_BOOL,
        )

    def test_command_type_positive(self):
        # type: () -> None
        """Positive command custom type test cases."""
        # string
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                strict: true
                namespace: type
                api_version: ""
                type: string
                fields:
                    foo: bar
            """)
        )

        # array of string
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                strict: true
                namespace: type
                api_version: ""
                type: array<string>
                fields:
                    foo: bar
            """)
        )

        # no fields
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                strict: true
                namespace: type
                api_version: ""
                type: string
            """)
        )

    def test_command_type_negative(self):
        # type: () -> None
        """Negative command type test cases."""

        # supports_doc_sequence must be a bool
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: type
                api_version: ""
                fields:
                    foo: bar
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
        )

    def test_stability_positive(self):
        # type: () -> None
        """Positive stability-field test cases."""
        for stability in ("stable", "unstable", "internal"):
            self.assert_parse(
                textwrap.dedent(f"""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: "1"
                    fields:
                        foo:
                            type: bar
                            stability: {stability}
                    reply_type: foo_reply_struct
                """)
            )

    def test_stability_negative(self):
        # type: () -> None
        """Negative stability-field test cases."""
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                namespace: ignored
                api_version: ""
                fields:
                    foo:
                        type: bar
                        stability: unstable
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_STABILITY_NO_API_VERSION,
        )
        self.assert_parse_fail(
            textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: "1"
                    fields:
                        foo:
                            type: bar
                            stability: "unknown"
                    reply_type: foo_reply_struct
                """),
            idl.errors.ERROR_ID_STABILITY_UNKNOWN_VALUE,
        )
        self.assert_parse_fail(
            textwrap.dedent("""
            commands:
                foo:
                    description: foo
                    command_name: foo
                    namespace: ignored
                    api_version: "1"
                    fields:
                        foo:
                            type: bar
                            unstable: true
                            stability: "unstable"
                    reply_type: foo_reply_struct
                """),
            idl.errors.ERROR_ID_DUPLICATE_UNSTABLE_STABILITY,
        )

    def test_scalar_or_mapping_negative(self):
        # type: () -> None
        """Negative test for scalar_or_mapping type."""

        # Test for scalar_or_mapping with a sequence.
        self.assert_parse_fail(
            textwrap.dedent("""
        server_parameters:
            foo:
                set_at: startup
                description: bar
                cpp_varname: baz
                redact: false
                default:
                - one
                - two
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE_SCALAR_OR_MAPPING,
        )

    def test_feature_flag(self):
        # type: () -> None
        """Test feature flag."""

        # Missing fcv_gated
        self.assert_parse_fail(
            textwrap.dedent("""
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: false
            """),
            idl.errors.ERROR_ID_MISSING_REQUIRED_FIELD,
        )

    def test_command_alias(self):
        # type: () -> None
        """Test the 'command_alis' field."""

        # The 'command_name' and 'command_alias' fields cannot have same value.
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                command_alias: foo
                namespace: ignored
                api_version: 1
                fields:
                    foo:
                        type: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_COMMAND_DUPLICATES_NAME_AND_ALIAS,
        )
        # The 'command_name' and 'command_alias' fields can be the same
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: bar
                command_alias: foo
                namespace: ignored
                api_version: 1
                fields:
                    foo:
                        type: bar
                reply_type: foo_reply_struct
            foofoo:
                description: foofoo
                command_name: foofoo
                command_alias: bar
                namespace: ignored
                api_version: 1
                fields:
                    foo:
                        type: bar
                reply_type: foo_reply_struct
            """),
        )

    def test_duplicate_command_names_allowed(self):
        # type: () -> None
        """Test that duplicate names (command_name, command defintition name) are allowed across different commands."""
        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo  
                namespace: ignored
                api_version: 1
                fields:
                    foo:
                        type: bar
                reply_type: foo_reply_struct
            bar:
                description: bar
                command_name: foo
                namespace: ignored
                api_version: 1
                fields:
                    baz:
                        type: bar
                reply_type: foo_reply_struct
            foofoo:
                description: foofoo
                command_name: foo 
                namespace: ignored
                api_version: 1
                fields:
                    baz:
                        type: bar
                reply_type: foo_reply_struct
            """)
        )
        self.assert_parse(
            textwrap.dedent("""
            commands:
                foo:  
                    description: first foo command
                    command_name: foo_command
                    namespace: ignored
                    api_version: 1
                    fields:
                        field1:
                            type: string
                    reply_type: foo_reply_struct
                foo:  
                    description: second foo command
                    command_name: bar_command 
                    namespace: ignored
                    api_version: 1
                    fields:
                        field2:
                            type: string
                    reply_type: foo_reply_struct
                foo:  # Third command with same name "foo"
                    description: third foo command
                    command_name: baz_command  
                    namespace: ignored
                    api_version: 1
                    fields:
                        field3:
                            type: string
                    reply_type: foo_reply_struct
            """)
        )

    def test_access_checks_positive(self):
        # type: () -> None
        """Positive access_check test cases."""

        self.assert_parse(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    ignore: true
                fields:
                    foo: bar
                reply_type: foo_reply_struct
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
                    none: true
                fields:
                    foo: bar
                reply_type: foo_reply_struct
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
                    simple:
                        check: is_authenticated
                fields:
                    foo: bar
                reply_type: foo_reply_struct
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
                            resource_pattern: foo
                            action_type: foo
                        - privilege:
                            resource_pattern: foo
                            action_type: foo
                        - privilege:
                            agg_stage: bar
                            resource_pattern: bar
                            action_type: bar
                        - check: is_authenticated
                fields:
                    foo: bar
                reply_type: foo_reply_struct
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
                    simple:
                        privilege:
                            resource_pattern: foo
                            action_type: foo
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """)
        )

    def test_access_checks_negative(self):
        # type: () -> None
        """Negative access_check test cases."""

        # check and privilege are present
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    simple:
                        check: foo
                        privilege:
                            resource_pattern: foo
                            action_type: foo
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_EITHER_CHECK_OR_PRIVILEGE,
        )

        # simple: true fails
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    simple: true
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_IS_NODE_TYPE,
        )

        # simple empty fails
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    simple: {}
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_EITHER_CHECK_OR_PRIVILEGE,
        )

        # duplicate access_check - none and simple
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    none: true
                    simple:
                        privilege:
                            resource_pattern: foo
                            action_type: foo
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_EMPTY_ACCESS_CHECK,
        )

        # duplicate access_check - none and complex
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    none: true
                    complex:
                        - privilege:
                            resource_pattern: foo
                            action_type: foo
                        - privilege:
                            resource_pattern: foo
                            action_type: foo
                        - check: is_authenticated
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_EMPTY_ACCESS_CHECK,
        )

        # duplicate access_check - simple and complex
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    simple:
                        privilege:
                            resource_pattern: foo
                            action_type: foo
                    complex:
                        - privilege:
                            resource_pattern: foo
                            action_type: foo
                        - privilege:
                            resource_pattern: foo
                            action_type: foo
                        - check: is_authenticated
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_EMPTY_ACCESS_CHECK,
        )

        # duplicate access_check - none, simple and complex
        self.assert_parse_fail(
            textwrap.dedent("""
        commands:
            foo:
                description: foo
                command_name: foo
                api_version: 1
                namespace: ignored
                access_check:
                    none: true
                    simple:
                        privilege:
                            resource_pattern: foo
                            action_type: foo
                    complex:
                        - privilege:
                            resource_pattern: foo
                            action_type: foo
                        - privilege:
                            resource_pattern: foo
                            action_type: foo
                        - check: is_authenticated
                fields:
                    foo: bar
                reply_type: foo_reply_struct
            """),
            idl.errors.ERROR_ID_EMPTY_ACCESS_CHECK,
        )

    def test_struct_unsafe_dangerous_disable_extra_field_duplicate_checks_negative(self):
        # Test commands and unsafe_dangerous_disable_extra_field_duplicate_checks are disallowed
        self.assert_parse_fail(
            textwrap.dedent("""
            commands:
                dangerc:
                    description: foo
                    namespace: ignored
                    command_name: dangerc
                    api_version: ""
                    strict: false
                    unsafe_dangerous_disable_extra_field_duplicate_checks: true
                    fields:
                        foo: string
            """),
            idl.errors.ERROR_ID_UNKNOWN_NODE,
        )


if __name__ == "__main__":
    unittest.main()
