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

import io
import os
import unittest
from textwrap import dedent

import yaml

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

    def test_server_parameter_constant_name(self) -> None:
        """Test generation of constants of server parameter names."""
        header, _ = self.assert_generate(
            self.view_test_common_types
            + dedent("""
        server_parameters:
                testServerParameter:
                    description: "Test server parameter"
                    set_at: ["startup", "runtime"]
                    redact: false
                    cpp_varname: testParameter
        """)
        )

        expected = dedent("constexpr inline auto kTestServerParameterName = \"testServerParameter\"_sd;")
        self.assertIn(expected, header)

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

    class IDLFile:
        """Reads an IDL file into a string to test code generation. Contains a utility that
        merges the IDL definitions in the passed in string with the IDL definitions read from
        file."""

        def __init__(self, testcase, path):
            if path is None or not os.path.exists(path):
                self._test_case.fail("IDLFileString must be constructed from a valid path")
            self._test_case = testcase
            with open(path, "r") as file:
                self._idl_content = file.read()

        def merge_with_idl_string(self, new_idl_string):
            """Merge the IDL definitions read from file with the IDL definitions in the string
            argument."""
            # Dictionary of (section name, list(class nodes)), holds info for `new_idl_string`
            section_to_defs = dict()
            # Dictionary of (section name, (section key node, section value node)), holds info
            # for `new_idl_string`
            section_to_nodes = dict()
            new_idl_node = yaml.compose(new_idl_string)
            self._test_case.assertTrue(new_idl_node)
            self._test_case.assertTrue(new_idl_node.id == "mapping")

            # Copy info from IDL string into dictionaries.
            for [section_key_node, section_value_node] in new_idl_node.value:
                section_name = section_key_node.value
                section_to_defs[section_name] = section_value_node.value
                section_to_nodes[section_name] = (section_key_node, section_value_node)

            root_node = yaml.compose(self._idl_content)
            self._test_case.assertTrue(root_node)
            self._test_case.assertTrue(root_node.id == "mapping")

            # For each section, find matching section in IDL string and merge.
            for [section_key_node, section_value_node] in root_node.value:
                section_name = section_key_node.value
                if section_name in section_to_defs:
                    section_value_node.value.extend(section_to_defs[section_name])
                    # Remove section to keep track of unmerged sections.
                    section_to_defs.pop(section_name)

            # Merge remaining sections from IDL string.
            for section_name in section_to_defs.keys():
                root_node.value.append(section_to_nodes[section_name])
            self._idl_content = yaml.serialize(root_node)

        def clear_structs_and_commands(self):
            """Clear struct and command definitions."""
            root_node = yaml.compose(self._idl_content)
            self._test_case.assertTrue(root_node)
            self._test_case.assertTrue(root_node.id == "mapping")

            for i in range(0, len(root_node.value)):
                section_key_node, section_value_node = root_node.value[i]
                # Make sure we're in section with class definitions.
                if section_key_node.value == "structs" or section_key_node.value == "commands":
                    section_value_node.value.clear()
            self._idl_content = yaml.serialize(root_node)

        def stream(self):
            return io.StringIO(str(self))

        def __str__(self):
            return self._idl_content

    def assertStringInFile(self, file_string, expected_string):
        self.assertIn(expected_string, file_string)

    def assertStringNotInFile(self, file_string, expected_string):
        self.assertNotIn(expected_string, file_string)

    def assertStringsInFile(self, file_string, expected_list):
        for expected_string in expected_list:
            self.assertStringInFile(file_string, expected_string)

    def assert_generate_with_basic_types(self, idl_defs):
        """Read in types from `basic_types.idl` and merge with the definitions in `idl_defs`."""
        basic_types_path = os.path.join(self._src_dir, "mongo", "db", "basic_types.idl")
        idl_file = self.IDLFile(self, basic_types_path)
        # Remove existing class definitions in `basic_types.idl` for structs and commands because
        # we want to test our own structs and commands in `idl_defs`.
        idl_file.clear_structs_and_commands()
        idl_file.merge_with_idl_string(idl_defs)
        return self.assert_generate(idl_file.stream())

    def test_validator_bind_integer_literals(self) -> None:
        """Test validator binds integer literals from validator expressions to integers."""

        # Binding integers in validator expressions greater than or equal to 2^31-1 should still
        # generate integer expressions rather than floating point expressions.
        _, source = self.assert_generate_with_basic_types("""
        structs:
                ValidatorStruct:
                    description: ValidatorStruct
                    fields:
                        validatorField:
                            type: long
                            validator:
                                gte: 2147483647
        """)

        expected = "static const std::int64_t rhs{2147483647};"
        self.assertIn(expected, source)

    def test_generate_namespace_string_dbname_constructor(self) -> None:
        """Test command constructors that initialize both NamespaceString and dbName fields
        use class members instead of constructor arguments to perform initialization (prevent
        use after move)."""
        _, source = self.assert_generate_with_basic_types("""
        commands:
                TestNamespaceDBNameCmd:
                    description: TestNamespaceDBNameCmd
                    command_name: testCmd
                    namespace: concatenate_with_db
                    api_version: ""
        """)
        self.assertStringsInFile(source, ["_nss(std::move(nss))", "_dbName(_nss.dbName())"])

    def test_variant_nested_struct(self) -> None:
        """Test that variants with structs as a possible argument pass the correct parser context
        and cast to the right type."""
        _, source = self.assert_generate_with_basic_types(
            dedent("""
        structs:
                NestedStruct:
                    description: "A struct which will be part of the variant type"
                    fields:
                        x:
                            type: int
                        y:
                            type: int
            
                VariantStruct:
                    description: "Struct which contains variant type with NestedStruct"
                    fields:
                        variantField:
                            type:
                                variant: [bool, NestedStruct]
                            optional: true
                                       
        """)
        )

        expectedParserContext = "IDLParserContext tempContext\(.*kVariantFieldFieldName.*\)"
        expectedCastedObject = "NestedStruct::parse\(localObject, tempContext.*\)"

        self.assertRegex(source, expectedParserContext)
        self.assertRegex(source, expectedCastedObject)

    def test_comparison_operators_generated(self) -> None:
        """Test the generation of 'generate_comparison_operators' and 'comparison_order'."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                ComparisonStruct:
                    description: Test comparison operators.
                    generate_comparison_operators: true
                    fields:
                        field1: int
                        field2: int
                        field3: int
            """
            )
        )
        self.assertStringsInFile(
            header,
            [
                "friend bool operator==(const ComparisonStruct& a, const ComparisonStruct& b)",
                "std::tuple(idl::relop::Ordering{_field1}, idl::relop::Ordering{_field2}, idl::relop::Ordering{_field3})",
            ],
        )

        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                ComparisonStruct:
                    description: Test comparison operators with specific comparison ordering.
                    generate_comparison_operators: true
                    fields:
                        field1:
                            type: int
                            comparison_order: 1
                        field2: int
                        field3:
                            type: int
                            comparison_order: 3
            """
            )
        )
        self.assertStringsInFile(
            header,
            [
                "friend bool operator==(const ComparisonStruct& a, const ComparisonStruct& b)",
                "std::tuple(idl::relop::Ordering{_field1}, idl::relop::Ordering{_field3})",
            ],
        )

    def test_is_reply_type(self) -> None:
        """Test generation specific to structs that are reply types."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                ReplyStruct:
                    description: Test struct that is a command reply.
                    is_command_reply: true
                    fields:
                        reply_field: int
            """
            )
        )
        self.assertStringInFile(header, "static constexpr bool _isCommandReply{true};")

    def test_required_field(self) -> None:
        """Test generation of 'required:' tag."""
        _, source = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                RequiredStruct:
                    description: Test struct with (implicit) strict field checking.
                    fields:
                        field1: int
            """
            )
        )
        self.assertStringInFile(source, "ctxt.throwUnknownField(fieldName)")
        _, source = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                NonRequiredStruct:
                    description: Test struct with non strict field checking.
                    strict: false
                    fields:
                        field1: int
            """
            )
        )
        self.assertStringNotInFile(source, "ctxt.throwUnknownField(fieldName)")

    def test_ignored_field(self) -> None:
        """Test generation of fields with 'ignore: true'."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                IgnoredStruct:
                    description: Test struct with an ignored_field.
                    fields:
                        ignored_field:
                            type: int
                            ignore: true
            """
            )
        )
        self.assertStringsInFile(
            header, ['static constexpr auto kIgnored_fieldFieldName = "ignored_field"_sd;']
        )
        self.assertStringNotInFile(header, "std::int32_t _ignored_field;")

    def test_default_values(self) -> None:
        """Test generation of the 'default:' tag."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                DefaultValuesStruct:
                    description: Test struct with default values.
                    fields:
                        stringField:
                            type: string
                            default: '"a default"'
                        intField:
                            type: int
                            default: 42
            """
            )
        )
        self.assertStringsInFile(
            header,
            [
                'std::string _stringField{"a default"};',
                "std::int32_t _intField{42};",
            ],
        )

    def test_optional_field(self) -> None:
        """Test generation of fields with 'optional: true'."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                OptionalStruct:
                    description: Test struct for an optional field.
                    fields:
                        field1:
                            type: string
                            optional: true
            """
            )
        )

        # Expect generation of boost::optional<T> when optional field is enabled.
        self.assertStringsInFile(
            header,
            [
                "boost::optional<std::string> _field1;",
            ],
        )

    def test_always_serialize(self) -> None:
        """Test generation of fields with 'always_serialize: true'."""
        _, source = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                AlwaysSerializeStruct:
                    description: Test for struct with always_serialize field.
                    fields:
                        field1:
                            type: string
                            always_serialize: true
                            optional: true
            """
            )
        )
        # Expect generation of appendNull in the case where field1 optional is uninitialized.
        self.assertStringsInFile(
            source,
            [
                "builder->appendNull(kField1FieldName);",
            ],
        )

    def test_enum(self) -> None:
        """Test generation of enum fields."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                EnumStruct:
                    description: Test for struct with an enum field.
                    fields:
                        field1: CollationCaseFirst
            """
            )
        )
        self.assertStringsInFile(
            header,
            [
                "mongo::CollationCaseFirstEnum _field1",
            ],
        )

    def test_cpp_validator(self) -> None:
        """Test generation of 'cpp_validator_func:' tag."""
        _, source = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                CppValidatorStruct:
                    description: Test for struct with always_serialize field.
                    cpp_validator_func: checkValuesEqual
                    fields:
                        field1: int
            """
            )
        )
        self.assertStringsInFile(
            source,
            [
                "checkValuesEqual(this)",
            ],
        )

    def test_variant_simple(self) -> None:
        """Test generation of variants uses std::variant."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                VariantStruct:
                    description: Test struct with single variant which accepts int or string.
                    fields:
                        value:
                            type:
                                variant: [int, string]
            """
            )
        )
        self.assertStringsInFile(
            header,
            [
                "std::variant<std::int32_t, std::string> _value;",
            ],
        )

    def test_chained_struct_variant(self) -> None:
        """Test generated chained structs with variants just generate normal chained structs."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                VariantStruct:
                    description: Struct with a variant.
                    strict: false
                    fields:
                        field1:
                            type:
                                variant:
                                    - string
                                    - int
                                    - bool
                ChainedVariantStruct:
                    description: Struct with chained variant.
                    inline_chained_structs: false
                    chained_structs:
                        VariantStruct: VariantStruct
            """
            )
        )
        self.assertStringsInFile(header, ["mongo::VariantStruct _variantStruct;"])
        # Check that we don't access the fields of `VariantStruct` directly.
        self.assertStringNotInFile(
            header,
            "return _variantStruct.getField1();",
        )
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                VariantStruct:
                    description: Struct with a variant.
                    strict: false
                    fields:
                        field1:
                            type:
                                variant:
                                    - string
                                    - int
                                    - bool
                ChainedVariantStruct:
                    description: Struct with chained variant.
                    inline_chained_structs: true
                    chained_structs:
                        VariantStruct: VariantStruct
            """
            )
        )
        self.assertStringsInFile(
            header,
            [
                "mongo::VariantStruct _variantStruct;",
                "const std::variant<std::string, std::int32_t, bool>& getField1() const { return _variantStruct.getField1(); }",
            ],
        )

    def test_callback_validators(self) -> None:
        """Test generation of validators with the 'callback:' tag."""
        _, source = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                CallbackValidatorStruct:
                    description: Test struct using fields with callback validators.
                    fields:
                        int_even:
                            type: int
                            validator: { callback: 'validateEvenNumber' }
            """
            )
        )
        self.assertStringsInFile(
            source,
            [
                dedent("""\
                        void CallbackValidatorStruct::validateInt_even(IDLParserContext& ctxt, const std::int32_t value)
                        {
                            uassertStatusOK(validateEvenNumber(value));
                        }""")
            ],
        )

    def test_chained_validators(self) -> None:
        """Test generation validators in chained structs."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                BasicValidatorStruct:
                    description: Struct using basic int range validator.
                    fields:
                        field1:
                            type: int
                            validator: { gt: 5 }
                ChainedValidatorStruct:
                    description: Uses a chained struct that includes field with validator.
                    chained_structs:
                        BasicValidatorStruct: BasicValidatorStruct
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["void validateField1(std::int32_t value)"],
        )

    def test_ignore_extra_duplicates(self) -> None:
        """Test generation of structs with 'unsafe_dangerous_disable_extra_field_duplicate_checks: true'."""
        _, source = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                ExtraCheckStruct:
                    description: Test struct (implicitly) enables checking extra field.
                    strict: false
                    fields:
                        field1: string
            """
            )
        )
        # Without either 'unsafe_dangerous_disable_extra_field_duplicate_checks: false' or
        # 'strict: true', a usedFieldSet is generated to check for extra field duplicates.
        self.assertStringInFile(
            source,
            "std::set<StringData> usedFieldSet;",
        )
        _, source = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                DisableExtraCheckStruct:
                    description: Test struct disables checking extra field.
                    unsafe_dangerous_disable_extra_field_duplicate_checks: false
                    fields:
                        field1: string
            """
            )
        )
        self.assertStringNotInFile(
            source,
            "std::set<StringData> usedFieldSet;",
        )

    def test_generic_arguments(self) -> None:
        """Test that structs tagged with 'is_generic_cmd_list: "arg"' generate as a chained struct
        on all commands"""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                GenericStruct:
                    description: Test struct of generic cmd list.
                    is_generic_cmd_list: "arg"
                    fields:
                        field1: int
                        field2: string
            commands:
                BasicCommand:
                    description: Test for a basic command includes generic args.
                    command_name: BasicCommand
                    namespace: ignored
                    api_version: ""
            """
            )
        )
        self.assertStringsInFile(
            header,
            [
                "const mongo::GenericStruct& getGenericStruct() const",
                "std::int32_t getField1() const",
                "StringData getField2() const",
                "mongo::GenericStruct _genericStruct;",
            ],
        )

    def test_concatenate_with_db_command(self) -> None:
        """Test generation command parameters taking a collection name."""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            commands:
                BasicConcatenateWithDbCommand:
                    description: Test for a basic concatenate_with_db command.
                    command_name: BasicConcatenateWithDbCommand
                    namespace: concatenate_with_db
                    api_version: ""
            """
            )
        )
        self.assertStringsInFile(
            header,
            [
                "NamespaceString _nss;",
            ],
        )
        self.assertStringsInFile(
            source,
            [
                "_nss = NamespaceStringUtil::deserialize(_dbName, collectionName);",
            ],
        )

    def test_supports_doc_sequence(self) -> None:
        """Test generation of fields with 'supports_doc_sequence: true'."""
        _, source = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                BasicStruct:
                    description: Test basic struct.
                    fields:
                        field1: int
            commands:
                DocSequenceCommand:
                    description: Test for a basic command with field marked with supports_doc_sequence.
                    command_name: DocSequenceCommand
                    namespace: ignored
                    api_version: ""
                    fields:
                        field1:
                            type: array<BasicStruct>
                            supports_doc_sequence: true
            """
            )
        )
        self.assertStringsInFile(
            source,
            [
                "for (auto&& sequence : request.sequences)",
                "values.emplace_back(mongo::BasicStruct::parse(sequenceObject, tempContext, dctx));",
            ],
        )

    def test_command_with_type_command_parameter(self) -> None:
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            commands:
                CommandTypeArrayObjectCommand:
                    description: Command with just an array of object parameter
                    command_name: CommandTypeArrayObjectCommand
                    namespace: type
                    api_version: ""
                    type: array<object>
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["std::vector<mongo::BSONObj> _commandParameter;"],
        )

    def test_command_with_reply_type(self) -> None:
        """Test generation of reply types for commands."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                ReplyStruct:
                    description: Test struct that is a reply type.
                    is_command_reply: true
            commands:
                CommandWithReplyType:
                    description: A command with its reply type specified by an IDL struct.
                    command_name: CommandWithReplyType
                    namespace: ignored
                    api_version: ""
                    reply_type: ReplyStruct
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["using Reply = mongo::ReplyStruct;"],
        )

    def test_stable_api_commands(self) -> None:
        """Test generation of commands with 'api_version: "1"' (non empty string)."""
        header, _ = self.assert_generate_with_basic_types(
            dedent(
                """
            structs:
                EmptyStruct:
                    description: Empty struct.
            commands:
                APIv1Command:
                    description: A Stable API command.
                    command_name: TestCommandName
                    namespace: ignored
                    strict: true
                    api_version: "1"
                    access_check:
                        none: true
                    reply_type: EmptyStruct
            """
            )
        )
        self.assertStringsInFile(
            header,
            [
                dedent("""\
                        template <typename Derived>
                        class TestCommandNameCmdVersion1Gen : public TypedCommand<Derived> {
                       """),
                "using Request = APIv1Command;",
            ],
        )

    def test_binary_compatible_feature_flag_disabled_by_default(self) -> None:
        """Test generation of a disabled by default binary-compatible feature flag"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: false
                    fcv_gated: false
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::BinaryCompatibleFeatureFlag gToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                "mongo::BinaryCompatibleFeatureFlag gToaster{false};",
                '<FeatureFlagServerParameter>("featureFlagToaster", &gToaster);',
            ],
        )

    def test_binary_compatible_feature_flag_enabled_by_default(self) -> None:
        """Test generation of an enabled by default binary-compatible feature flag"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: true
                    fcv_gated: false
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::BinaryCompatibleFeatureFlag gToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                "mongo::BinaryCompatibleFeatureFlag gToaster{true};",
                '<FeatureFlagServerParameter>("featureFlagToaster", &gToaster);',
            ],
        )

    def test_fcv_gated_feature_flag_disabled_on_all_versions_by_default(self) -> None:
        """Test generation of an FCV-gated feature flag disabled by default for all versions"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: false
                    fcv_gated: true
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::FCVGatedFeatureFlag gToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                'mongo::FCVGatedFeatureFlag gToaster{false, ""_sd};',
                '<FeatureFlagServerParameter>("featureFlagToaster", &gToaster);',
            ],
        )

    def test_fcv_gated_feature_flag_enabled_since_version(self) -> None:
        """Test generation of an FCV-gated feature flag enabled since a specified version"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            feature_flags:
                featureFlagToaster:
                    description: >
                      Make toast
                      (Enable on transitional FCV): Lorem ipsum dolor sit amet
                    cpp_varname: gToaster
                    default: true
                    version: 123
                    fcv_gated: true
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::FCVGatedFeatureFlag gToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                'mongo::FCVGatedFeatureFlag gToaster{true, "123"_sd};',
                '<FeatureFlagServerParameter>("featureFlagToaster", &gToaster);',
            ],
        )

    def test_fcv_gated_feature_flag_disabled_on_all_versions_by_default_with_enable_on_transitional_fcv(
        self,
    ) -> None:
        """Test generation of an FCV-gated feature flag that specifies to transition on kUpgrading"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
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
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::FCVGatedFeatureFlag gToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                'mongo::FCVGatedFeatureFlag gToaster{true, "123"_sd, true};',
                '<FeatureFlagServerParameter>("featureFlagToaster", &gToaster);',
            ],
        )

    def test_fcv_gated_feature_flag_disabled_on_all_versions_by_default_with_enable_on_transitional_fcv_false(
        self,
    ) -> None:
        """Test that the generation of an FCV-gated feature flag that specifies enable_on_transitional_fcv_UNSAFE: false is equivalent to the default"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    default: true
                    version: 123
                    fcv_gated: true
                    enable_on_transitional_fcv_UNSAFE: false
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::FCVGatedFeatureFlag gToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                'mongo::FCVGatedFeatureFlag gToaster{true, "123"_sd};',
                '<FeatureFlagServerParameter>("featureFlagToaster", &gToaster);',
            ],
        )

    def test_legacy_context_unaware_fcv_gated_feature_flag(self) -> None:
        """Test generation of an FCV-gated feature flag that uses the legacy feature flag API"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            feature_flags:
                featureFlagLegacyAPIToaster:
                    description: "Make toast"
                    cpp_varname: gLegacyAPIToaster
                    default: true
                    version: 123
                    fcv_gated: true
                    fcv_context_unaware: true
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::LegacyContextUnawareFCVGatedFeatureFlag gLegacyAPIToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                'mongo::LegacyContextUnawareFCVGatedFeatureFlag gLegacyAPIToaster{true, "123"_sd};',
                '<FeatureFlagServerParameter>("featureFlagLegacyAPIToaster", &gLegacyAPIToaster);',
            ],
        )

    def test_in_development_incremental_feature_rollout_flag(self) -> None:
        """Test generation of an Incremental Feature Rollout (IFR) feature flag"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: in_development
                    fcv_gated: false
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::IncrementalRolloutFeatureFlag gToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                "mongo::IncrementalRolloutFeatureFlag gToaster{"
                + '"featureFlagToaster"_sd, RolloutPhase::inDevelopment, false};',
                '<FeatureFlagServerParameter>("featureFlagToaster", &gToaster);',
            ],
        )

    def test_rollout_incremental_feature_rollout_flag(self) -> None:
        """Test generation of an Incremental Feature Rollout (IFR) feature flag"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: rollout
                    fcv_gated: false
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::IncrementalRolloutFeatureFlag gToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                "mongo::IncrementalRolloutFeatureFlag gToaster{"
                + '"featureFlagToaster"_sd, RolloutPhase::rollout, true};',
                '<FeatureFlagServerParameter>("featureFlagToaster", &gToaster);',
            ],
        )

    def test_released_incremental_feature_rollout_flag(self) -> None:
        """Test generation of an Incremental Feature Rollout (IFR) feature flag"""
        header, source = self.assert_generate_with_basic_types(
            dedent(
                """
            feature_flags:
                featureFlagToaster:
                    description: "Make toast"
                    cpp_varname: gToaster
                    incremental_rollout_phase: released
                    fcv_gated: false
            """
            )
        )
        self.assertStringsInFile(
            header,
            ["mongo::IncrementalRolloutFeatureFlag gToaster;"],
        )
        self.assertStringsInFile(
            source,
            [
                "mongo::IncrementalRolloutFeatureFlag gToaster{"
                + '"featureFlagToaster"_sd, RolloutPhase::released, true};',
                '<FeatureFlagServerParameter>("featureFlagToaster", &gToaster);',
            ],
        )


if __name__ == "__main__":
    unittest.main()
