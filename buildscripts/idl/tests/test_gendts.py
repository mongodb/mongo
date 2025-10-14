#!/usr/bin/env python3
#
# Copyright (C) 2024-present MongoDB, Inc.
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
Test cases for Typescript declaration generator.
"""

import os
import sys
import unittest
from contextlib import contextmanager
from io import StringIO
from types import SimpleNamespace
from typing import DefaultDict, Iterable, Tuple

# import package so that it works regardless of whether we run as a module or file
if __package__ is None:
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    import testcase
else:
    from . import testcase

# Permit imports from "buildscripts".
sys.path.append(os.path.normpath(os.path.join(os.path.abspath(__file__), "../../../../")))
from buildscripts.idl.gen_dts import gen_dts, object_to_dts, parser, syntax
from buildscripts.idl.idl.compiler import CompilerImportResolver


class TestGenDTS(testcase.IDLTestcase):
    """Test the Typescript declaration Generator."""

    def _parse_idl(self, idl: str) -> syntax.IDLParsedSpec:
        """Parses an IDL string."""
        self.assert_parse(idl, CompilerImportResolver(["src"]))
        return parser.parse(StringIO(idl), "", CompilerImportResolver(["src"]))

    def _test_object_to_dts(self, idl: Iterable[str], expected_result: Iterable[str]) -> None:
        """Tests if object_to_dts returned an expected result from parsing an IDL string."""
        symbol_table = self._parse_idl("\n".join(idl)).spec.symbols
        outputs = []
        for symbol_type in ("structs", "commands", "enums"):
            outputs += (
                object_to_dts(obj)
                for obj in symbol_table.__getattribute__(symbol_type)
                if not obj.imported
            )
        self.assertEqual("\n".join(outputs), "\n".join(expected_result))

    @staticmethod
    def _create_mock_filesystem():
        """Creates a mock filesystem that implements some IO functions."""

        class MockFilesystem:
            def __init__(self) -> None:
                self.files = DefaultDict(lambda: "")

            @contextmanager
            def open(self, path: str, mode: str):
                def write(text):
                    self.files[path] += f"{text}"

                def readlines():
                    lines = self.files[path].split("\n")
                    return [f"{line}\n" for line in lines]

                yield (
                    SimpleNamespace(write=write)
                    if mode in "wa"
                    else SimpleNamespace(readlines=readlines)
                )

            def exists(self, path: str):
                return path in self.files

        return MockFilesystem()

    def _test_multiple_idls(
        self,
        idls: Iterable[Iterable[str]],
        *,
        structs: Iterable[str] = tuple(),
        commands: Iterable[str] = tuple(),
        enums: Iterable[str] = tuple(),
    ) -> None:
        """Tests if multiple IDL files generate an expected output across the 3 generated files."""
        declarations = {"structs": set(), "enums": set(), "commands": set()}
        mock_fs = TestGenDTS._create_mock_filesystem()
        for idl in idls:
            idl = self._parse_idl("\n".join(idl))
            gen_dts(
                idl, mock_fs.open, mock_fs.exists, ignore_imported=True, existing_decls=declarations
            )
        self.assertEqual(mock_fs.files["src/mongo/shell/structs_gen.d.ts"], "\n".join(structs))
        self.assertEqual(mock_fs.files["src/mongo/shell/enums_gen.d.ts"], "\n".join(enums))
        self.assertEqual(mock_fs.files["src/mongo/shell/commands_gen.d.ts"], "\n".join(commands))

    def _get_generic_idl_with_header(self) -> Tuple[str]:
        """Returns a header necessary to use commands in an IDL."""
        return ("global:", "    cpp_namespace: 'mongo'", "", "imports: []", "")

    def _get_command_idl_text(
        self, name: str, description: str, namespace: str = "ignored"
    ) -> Tuple[str]:
        """Returns a command IDL declaration with a given name, description, and namespace."""
        return (
            "commands:",
            f"   {name}:",
            "       command_name: ' '",
            "       cpp_name: ' '",
            f"       description: '{description}'",
            "       api_version: ''",
            f"       namespace: '{namespace}'",
        )

    def _get_command_idl_with_header(
        self, name: str, description: str, namespace: str = "ignored"
    ) -> Tuple[str]:
        """Returns a command IDL declaration with a given name, description, and namespace, with the necessary headers."""
        return (
            "global:",
            '    cpp_namespace: "mongo"',
            "",
            "imports: []",
            "",
            *self._get_command_idl_text(name, description, namespace),
        )

    def test_empty_struct(self):
        self._test_object_to_dts(
            (
                "structs:",
                "   Foo:",
                "       description: foo",
            ),
            ("/** foo */", "type Foo = {", "", "};"),
        )

    def test_string_enum(self):
        self._test_object_to_dts(
            (
                "enums:",
                "   Foo:",
                "        description: ''",
                "        type: string",
                "        values:",
                "            v0: v0",
                "            v1: v1",
                "            v2: v2",
            ),
            (('type Foo = "v0" | "v1" | "v2";'),),
        )

    def test_int_enum(self):
        self._test_object_to_dts(
            (
                "enums:",
                "   Foo:",
                "        description: ''",
                "        type: int",
                "        values:",
                "            v0: 0",
                "            v1: 1",
                "            v2: 2",
            ),
            (('type Foo = "0" | "1" | "2";'),),
        )

    def test_empty_command(self):
        self._test_object_to_dts(
            self._get_command_idl_with_header(name="Foo", description="foo"),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo */",
                "\t\tFoo: 1;",
                "",
                "",
                "\t}, object>",
                "};",
            ),
        )

    def test_command_namespaces(self):
        self._test_object_to_dts(
            (
                *self._get_command_idl_with_header(name="Foo", description="foo", namespace="type"),
                "       type: array<string>",
            ),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo */",
                "\t\tFoo: (string)[];",
                "",
                "",
                "\t}, object>",
                "};",
            ),
        )
        self._test_object_to_dts(
            self._get_command_idl_with_header(name="Foo", description="foo", namespace=""),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo */",
                "\t\tFoo: NamespaceString;",
                "",
                "",
                "\t}, object>",
                "};",
            ),
        )

    def test_command_with_single_field_without_description(self):
        self._test_object_to_dts(
            (
                *self._get_command_idl_with_header(name="Foo", description="foo"),
                "       fields:",
                "           bar: string",
            ),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo",
                "\t\t(Fields: `bar`) */",
                "\t\tFoo: 1;",
                "",
                "\t\t/** (field) */",
                "\t\tbar: string;",
                "",
                "\t}, object>",
                "};",
            ),
        )

    def test_command_with_single_field_with_description(self):
        self._test_object_to_dts(
            (
                *self._get_command_idl_with_header(name="Foo", description="foo"),
                "       fields:",
                "           bar:",
                "               type: string",
                "               description: bar field",
            ),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo",
                "\t\t(Fields: `bar`) */",
                "\t\tFoo: 1;",
                "",
                "\t\t/** (field) bar field */",
                "\t\tbar: string;",
                "",
                "\t}, object>",
                "};",
            ),
        )

    def test_command_with_array_field(self):
        self._test_object_to_dts(
            (
                *self._get_command_idl_with_header(name="Foo", description="foo"),
                "       fields:",
                "           bar:",
                "               description: bar field",
                "               type: array<int64>",
            ),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo",
                "\t\t(Fields: `bar`) */",
                "\t\tFoo: 1;",
                "",
                "\t\t/** (field) bar field */",
                "\t\tbar: (int64)[];",
                "",
                "\t}, object>",
                "};",
            ),
        )

    def test_command_with_variant_field(self):
        self._test_object_to_dts(
            (
                *self._get_command_idl_with_header(name="Foo", description="foo"),
                "       fields:",
                "           bar:",
                "               description: bar field",
                "               type: ",
                "                   variant:",
                "                       - int64",
                "                       - bool",
                "                       - double",
            ),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo",
                "\t\t(Fields: `bar`) */",
                "\t\tFoo: 1;",
                "",
                "\t\t/** (field) bar field */",
                "\t\tbar: int64|bool|double;",
                "",
                "\t}, object>",
                "};",
            ),
        )

    def test_command_with_variant_of_arrays(self):
        self._test_object_to_dts(
            (
                *self._get_command_idl_with_header(name="Foo", description="foo"),
                "       fields:",
                "           bar:",
                "               description: bar field",
                "               type: ",
                "                   variant:",
                "                       - int64",
                "                       - array<bool>",
                "                       - double",
            ),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo",
                "\t\t(Fields: `bar`) */",
                "\t\tFoo: 1;",
                "",
                "\t\t/** (field) bar field */",
                "\t\tbar: int64|(bool)[]|double;",
                "",
                "\t}, object>",
                "};",
            ),
        )

    def test_command_with_optional_fields(self):
        self.maxDiff = None
        self._test_object_to_dts(
            (
                *self._get_command_idl_with_header(name="Foo", description="foo"),
                "       fields:",
                "           bar0:",
                "               description: bar0 field",
                "               type: int64",
                "               optional: true",
                "           bar1:",
                "               description: bar1 field",
                "               type: double",
                "               default: -1",
                "           bar2:",
                "               description: bar2 field",
                "               type: optional_bool",
                "           baz:",
                "               description: baz field",
                "               type: float",
            ),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo",
                "\t\t(Fields: `baz` `bar0?` `bar1?` `bar2?`) */",
                "\t\tFoo: 1;",
                "",
                "\t\t/** (field) bar0 field */",
                "\t\tbar0?: int64;",
                "",
                "\t\t/** (field) bar1 field */",
                "\t\tbar1?: double;",
                "",
                "\t\t/** (field) bar2 field */",
                "\t\tbar2?: optional_bool;",
                "",
                "\t\t/** (field) baz field */",
                "\t\tbaz: float;",
                "",
                "\t}, object>",
                "};",
            ),
        )

    def test_command_reply_types(self):
        self._test_object_to_dts(
            (
                *self._get_command_idl_with_header(name="Foo", description="foo"),
                "       reply_type: int",
            ),
            (
                "interface Commands {",
                "",
                "\t/** foo */",
                "\tFoo: Command<{",
                "",
                "\t\t/** (command) foo */",
                "\t\tFoo: 1;",
                "",
                "",
                "\t}, int>",
                "};",
            ),
        )

    def test_one_idl_multiple_declarations(self):
        self._test_multiple_idls(
            (
                (
                    *self._get_generic_idl_with_header(),
                    "structs:",
                    "   FooStruct:",
                    "       description: foostruct",
                    "",
                    "enums:",
                    "   FooEnum:",
                    "        description: 'fooenum'",
                    "        type: string",
                    "        values:",
                    "            v0: v0",
                    "            v1: v1",
                    "            v2: v2",
                    "",
                    *self._get_command_idl_text(name="FooCmd", description="foocmd"),
                ),
            ),
            structs=(
                "/** foostruct */",
                "type FooStruct = {",
                "",
                "};",
                "",
                "",
            ),
            enums=('type FooEnum = "v0" | "v1" | "v2";', "", ""),
            commands=(
                "interface Commands {",
                "",
                "\t/** foocmd */",
                "\tFooCmd: Command<{",
                "",
                "\t\t/** (command) foocmd */",
                "\t\tFooCmd: 1;",
                "",
                "",
                "\t}, object>",
                "};",
                "",
                "",
            ),
        )

    def test_multiple_idls_each_with_one_declaration(self):
        self._test_multiple_idls(
            (
                (
                    "structs:",
                    "   FooStruct:",
                    "       description: foostruct",
                ),
                (
                    "enums:",
                    "   FooEnum:",
                    "        description: 'fooenum'",
                    "        type: string",
                    "        values:",
                    "            v0: v0",
                    "            v1: v1",
                    "            v2: v2",
                    "",
                ),
                self._get_command_idl_with_header(name="FooCmd", description="foocmd"),
            ),
            structs=(
                "/** foostruct */",
                "type FooStruct = {",
                "",
                "};",
                "",
                "",
            ),
            enums=('type FooEnum = "v0" | "v1" | "v2";', "", ""),
            commands=(
                "interface Commands {",
                "",
                "\t/** foocmd */",
                "\tFooCmd: Command<{",
                "",
                "\t\t/** (command) foocmd */",
                "\t\tFooCmd: 1;",
                "",
                "",
                "\t}, object>",
                "};",
                "",
                "",
            ),
        )

    def test_multiple_idls_avoid_redeclaration(self):
        self._test_multiple_idls(
            (
                (
                    "structs:",
                    "   FooStruct:",
                    "       description: foostruct",
                ),
                (
                    "structs:",
                    "   FooStruct:",
                    "       description: foostruct",
                ),
            ),
            structs=(
                "/** foostruct */",
                "type FooStruct = {",
                "",
                "};",
                "",
                "",
            ),
        )


if __name__ == "__main__":
    unittest.main()
