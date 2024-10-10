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
"""
Test cases for IDL Generator.

This file exists to verify code coverage for the generator.py file. To run code coverage, run in the
idl base directory:

$ coverage run run_tests.py && coverage html
"""

import os
import unittest
from textwrap import dedent

# import package so that it works regardless of whether we run as a module or file
if __package__ is None:
    import sys

    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    import testcase
    from context import idl
else:
    from . import testcase
    from .context import idl


class TestGenerator(testcase.IDLTestcase):
    """Test the IDL Generator."""

    output_suffix = "_codecoverage_gen"
    idl_files_to_test = ["unittest", "unittest_import"]

    @property
    def _src_dir(self):
        """Get the directory of the src folder."""
        base_dir = os.path.dirname(
            os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        )
        return os.path.join(
            base_dir,
            "src",
        )

    @property
    def _idl_dir(self):
        """Get the directory of the idl folder."""
        return os.path.join(self._src_dir, "mongo", "idl")

    def tearDown(self) -> None:
        """Cleanup resources created by tests."""
        for idl_file in self.idl_files_to_test:
            for ext in ["h", "cpp"]:
                file_path = os.path.join(self._idl_dir, f"{idl_file}{self.output_suffix}.{ext}")
                if os.path.exists(file_path):
                    os.remove(file_path)

    def test_compile(self):
        # type: () -> None
        """Exercise the code generator so code coverage can be measured."""
        args = idl.compiler.CompilerArgs()
        args.output_suffix = self.output_suffix
        args.import_directories = [self._src_dir]

        unittest_idl_file = os.path.join(self._idl_dir, f"{self.idl_files_to_test[0]}.idl")
        if not os.path.exists(unittest_idl_file):
            unittest.skip(
                "Skipping IDL Generator testing since %s could not be found." % (unittest_idl_file)
            )
            return

        for idl_file in self.idl_files_to_test:
            args.input_file = os.path.join(self._idl_dir, f"{idl_file}.idl")
            self.assertTrue(idl.compiler.compile_idl(args))

    def test_enum_non_const(self):
        # type: () -> None
        """Validate enums are not marked as const in getters."""
        header, _ = self.assert_generate("""
        types:
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false

        enums:

            StringEnum:
                description: "An example string enum"
                type: string
                values:
                    s0: "zero"
                    s1: "one"
                    s2: "two"

        structs:
            one_string_enum:
                description: mock
                fields:
                    value: StringEnum
        """)

        # Look for the getter.
        # Make sure the getter is marked as const.
        # Make sure the return type is not marked as const by validating the getter marked as const
        # is the only occurrence of the word "const".
        header_lines = header.split("\n")

        found = False
        for header_line in header_lines:
            if (
                header_line.find("getValue") > 0
                and header_line.find("const {") > 0
                and header_line.find("const {") == header_line.find("const")
            ):
                found = True

        self.assertTrue(found, "Bad Header: " + header)

    def test_custom_array_type_function_serialization(self) -> None:
        """Test the function based serialization codegen for array containing custom types."""
        _, source = self.assert_generate("""
        types:
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false

                Pokemon:
                  description: "Yet another custom type"
                  cpp_type: "stdx::variant<Pikachu, Snorlax>"
                  bson_serialization_type: any
                  serializer: ::pokemon::pokedex::record
                  deserializer: ::pokemon::pokedex::lookup
                  is_view: false

        structs:
                Pokedex:
                  description: "Struct representing the index hint spec."
                  fields:
                    knownPokemons:
                      type: array<Pokemon>
        """)
        expected = dedent("""
        void Pokedex::serialize(BSONObjBuilder* builder) const {
            _hasMembers.required();
        
            {
                BSONArrayBuilder arrayBuilder(builder->subarrayStart(kKnownPokemonsFieldName));
                for (const auto& item : _knownPokemons) {
                    ::pokemon::pokedex::record(item, &arrayBuilder);
                }
            }
        
        }
        """)
        self.assertIn(expected, source)

    def test_custom_array_type_method_serialization(self) -> None:
        """Test the method based serialization codegen for array containing custom types."""
        _, source = self.assert_generate("""
        types:
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false

                Pokemon:
                  description: "Yet another custom type"
                  cpp_type: "stdx::variant<Pikachu, Snorlax>"
                  bson_serialization_type: any
                  serializer: record
                  deserializer: lookup
                  is_view: false

        structs:
                Pokedex:
                  description: "Struct representing the index hint spec."
                  fields:
                    knownPokemons:
                      type: array<Pokemon>
        """)
        expected = dedent("""
        void Pokedex::serialize(BSONObjBuilder* builder) const {
            _hasMembers.required();
        
            {
                BSONArrayBuilder arrayBuilder(builder->subarrayStart(kKnownPokemonsFieldName));
                for (const auto& item : _knownPokemons) {
                    item.record(&arrayBuilder);
                }
            }
        
        }
        """)
        self.assertIn(expected, source)

    def test_object_type_with_custom_serializer_and_query_shape_specification_custom(self) -> None:
        """Serialization with custom query_shape."""
        _, source = self.assert_generate("""
        types:
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false

                object_type_with_custom_serializer:
                    bson_serialization_type: object
                    description: ObjWithCustomSerializer
                    cpp_type: ObjWithCustomSerializer
                    serializer: ObjWithCustomSerializer::toBSON
                    deserializer: ObjWithCustomSerializer::parse
                    is_view: false

        structs:
                QueryShapeSpec:
                    description: QueryShape
                    query_shape_component: true
                    fields:
                        internalObject:
                            type: object_type_with_custom_serializer
                            optional: false
                            description: internalObject
                            query_shape: custom
        """)

        expected = dedent("""
        void QueryShapeSpec::serialize(BSONObjBuilder* builder, const SerializationOptions& options) const {
            _hasMembers.required();

            {
                const BSONObj localObject = _internalObject.toBSON(options);
                builder->append(kInternalObjectFieldName, localObject);
            }

        }""")
        self.assertIn(expected, source)

    def test_array_of_object_type_with_custom_serializer_and_query_shape_specification_custom(
        self,
    ) -> None:
        """Serialization with custom query_shape used, array use case."""
        _, source = self.assert_generate("""
        types:
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false

                object_type_with_custom_serializer:
                    bson_serialization_type: object
                    description: ObjWithCustomSerializer
                    cpp_type: ObjWithCustomSerializer
                    serializer: ObjWithCustomSerializer::toBSON
                    deserializer: ObjWithCustomSerializer::parse
                    is_view: false

        structs:
                QueryShapeSpec:
                    description: QueryShape
                    query_shape_component: true
                    fields:
                        internalObjectArray:
                            type: array<object_type_with_custom_serializer>
                            optional: false
                            description: internalObjectArray
                            query_shape: custom
        """)

        expected = dedent("""
        void QueryShapeSpec::serialize(BSONObjBuilder* builder, const SerializationOptions& options) const {
            _hasMembers.required();

            {
                BSONArrayBuilder arrayBuilder(builder->subarrayStart(kInternalObjectArrayFieldName));
                for (const auto& item : _internalObjectArray) {
                    const BSONObj localObject = item.toBSON(options);
                    arrayBuilder.append(localObject);
                }
            }

        }""")
        self.assertIn(expected, source)

    view_test_common_types = dedent("""
        types:
                serialization_context:
                    bson_serialization_type: any
                    description: foo
                    cpp_type: foo
                    internal_only: true
                    is_view: false

                object_is_view:
                    bson_serialization_type: object
                    description: ObjIsView
                    cpp_type: ObjIsView
                    serializer: ObjIsView::toBSON
                    deserializer: ObjIsView::parse
                    is_view: true
                
                object_is_not_view:
                    bson_serialization_type: object
                    description: ObjIsView
                    cpp_type: ObjIsView
                    serializer: ObjIsView::toBSON
                    deserializer: ObjIsView::parse
                    is_view: false
                
                random_type_not_view:
                    bson_serialization_type: any
                    description: RandomType
                    cpp_type: RandomType
                    serializer: RandomType::toBSON
                    deserializer: RandomType::parse
                    is_view: false
                
                tenant_id:
                    bson_serialization_type: any
                    description: "A struct representing a tenant id"
                    cpp_type: "TenantId"
                    deserializer: "mongo::TenantId::parseFromBSON"
                    serializer: "mongo::TenantId::serializeToBSON"
                    is_view: false

                database_name:
                    bson_serialization_type: string
                    description: "A MongoDB DatabaseName"
                    cpp_type: "mongo::DatabaseName"
                    serializer: "::mongo::DatabaseNameUtil::serialize"
                    deserializer: "::mongo::DatabaseNameUtil::deserialize"
                    deserialize_with_tenant: true
                    is_view: false
                
                bool:
                    bson_serialization_type: bool
                    description: "A BSON bool"
                    cpp_type: "bool"
                    deserializer: "mongo::BSONElement::boolean"
                    is_view: false
    """)

    def test_view_struct_generates_anchor(self) -> None:
        """Test anchor generation on view struct."""
        header, _ = self.assert_generate(
            self.view_test_common_types
            + dedent("""
        structs:
                ViewStruct:
                    description: ViewStruct
                    fields:
                        value1: object_is_view
                        value2: random_type_not_view
                        value3: random_type_not_view
                        value4: random_type_not_view
        """)
        )

        expected = dedent("BSONObj _anchorObj;")
        self.assertIn(expected, header)

    def test_non_view_struct_does_not_generate_anchor(self) -> None:
        """Test anchor is not generated on non view struct."""
        header, _ = self.assert_generate(
            self.view_test_common_types
            + dedent("""
        structs:
                NonViewStruct:
                    description: NonViewStruct
                    fields:
                        value1: object_is_not_view
                        value2: random_type_not_view
                        value3: random_type_not_view
                        value4: random_type_not_view
        """)
        )

        expected = dedent("BSONObj _anchorObj;")
        self.assertNotIn(expected, header)

    def test_compound_view_struct_generates_anchor(self) -> None:
        """Test anchor generation on view struct with compound type."""
        header, _ = self.assert_generate(
            self.view_test_common_types
            + dedent("""
        structs:
                ViewStruct:
                    description: ViewStruct
                    fields:
                        value1: array<object_is_view>
                        value2: random_type_not_view
                        value3: random_type_not_view
                        value4: random_type_not_view
        """)
        )

        expected = dedent("BSONObj _anchorObj;")
        self.assertIn(expected, header)

    def test_compound_non_view_struct_does_not_generate_anchor(self) -> None:
        """Test anchor is not generated on non view struct with compound type."""
        header, _ = self.assert_generate(
            self.view_test_common_types
            + dedent("""
        structs:
                NonViewStruct:
                    description: NonViewStruct
                    fields:
                        value1: array<object_is_not_view>
                        value2: random_type_not_view
                        value3: random_type_not_view
                        value4: random_type_not_view
        """)
        )

        expected = dedent("BSONObj _anchorObj;")
        self.assertNotIn(expected, header)

    def test_command_view_type_generates_anchor(self) -> None:
        """Test anchor generation on command with view parameter."""
        header, _ = self.assert_generate(
            self.view_test_common_types
            + dedent("""
        commands:
                CommandTypeArrayObjectCommand:
                    description: CommandTypeArrayObjectCommand
                    command_name: CommandTypeArrayObjectCommand
                    namespace: type
                    api_version: ""
                    type: array<object_is_view>
        """)
        )

        expected = dedent("BSONObj _anchorObj;")
        self.assertIn(expected, header)

    def test_command_non_view_type_does_not_generate_anchor(self) -> None:
        """Test anchor is not generated on command with nont view parameter."""
        header, _ = self.assert_generate(
            self.view_test_common_types
            + dedent("""
        commands:
                CommandTypeArrayObjectCommand:
                    description: CommandTypeArrayObjectCommand
                    command_name: CommandTypeArrayObjectCommand
                    namespace: type
                    api_version: ""
                    type: array<object_is_not_view>
        """)
        )

        expected = dedent("BSONObj _anchorObj;")
        self.assertNotIn(expected, header)

    def test_chained_view_struct_generates_anchor(self) -> None:
        """Test anchor generation on struct chained with view struct."""
        header, _ = self.assert_generate(
            self.view_test_common_types
            + dedent("""
        structs:
                ViewStruct:
                    description: ViewStruct
                    fields:
                        value1: array<object_is_view>
                        value2: random_type_not_view
                        value3: random_type_not_view
                        value4: random_type_not_view
                ViewStructChainedStruct:
                    description: ViewStructChainedStruct
                    chained_structs:
                        ViewStruct: ViewStruct
        """)
        )

        expected = dedent("BSONObj _anchorObj;")
        self.assertIn(expected, header)

    def test_chained_non_view_struct_does_not_generate_anchor(self) -> None:
        """Test anchor not generated on struct chained with non view struct."""
        header, _ = self.assert_generate(
            self.view_test_common_types
            + dedent("""
        structs:
                NonViewStruct:
                    description: NonViewStruct
                    fields:
                        value1: array<object_is_not_view>
                        value2: random_type_not_view
                        value3: random_type_not_view
                        value4: random_type_not_view
                NonViewStructChainedStruct:
                    description: NonViewStructChainedStruct
                    chained_structs:
                        NonViewStruct: NonViewStruct
        """)
        )

        expected = dedent("BSONObj _anchorObj;")
        self.assertNotIn(expected, header)


if __name__ == "__main__":
    unittest.main()
