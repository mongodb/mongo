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
    from context import idl
    import testcase
else:
    from .context import idl
    from . import testcase


class TestGenerator(testcase.IDLTestcase):
    """Test the IDL Generator."""

    output_suffix = "_codecoverage_gen"
    idl_files_to_test = ["unittest", "unittest_import"]

    @property
    def _src_dir(self):
        """Get the directory of the src folder."""
        base_dir = os.path.dirname(
            os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
        return os.path.join(
            base_dir,
            'src',
        )

    @property
    def _idl_dir(self):
        """Get the directory of the idl folder."""
        return os.path.join(self._src_dir, 'mongo', 'idl')

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

        unittest_idl_file = os.path.join(self._idl_dir, f'{self.idl_files_to_test[0]}.idl')
        if not os.path.exists(unittest_idl_file):
            unittest.skip(
                "Skipping IDL Generator testing since %s could not be found." % (unittest_idl_file))
            return

        for idl_file in self.idl_files_to_test:
            args.input_file = os.path.join(self._idl_dir, f'{idl_file}.idl')
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
        header_lines = header.split('\n')

        found = False
        for header_line in header_lines:
            if header_line.find("getValue") > 0 \
                and header_line.find("const {") > 0 \
                and header_line.find("const {") == header_line.find("const"):
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

                Pokemon:
                  description: "Yet another custom type"
                  cpp_type: "stdx::variant<Pikachu, Snorlax>"
                  bson_serialization_type: any
                  serializer: ::pokemon::pokedex::record
                  deserializer: ::pokemon::pokedex::lookup

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

                Pokemon:
                  description: "Yet another custom type"
                  cpp_type: "stdx::variant<Pikachu, Snorlax>"
                  bson_serialization_type: any
                  serializer: record
                  deserializer: lookup

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


if __name__ == '__main__':

    unittest.main()
