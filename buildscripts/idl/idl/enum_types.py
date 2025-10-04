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
IDL Enum type information.

Support the code generation for enums
"""

import json
from abc import ABCMeta, abstractmethod
from typing import cast

import bson

from . import ast, common, writer


class EnumTypeInfoBase(object, metaclass=ABCMeta):
    """Base type for enumeration type information."""

    def __init__(self, idl_enum):
        # type: (Union[syntax.Enum,ast.Enum]) -> None
        """Construct a EnumTypeInfoBase."""
        self._enum = idl_enum

    def get_qualified_cpp_type_name(self):
        # type: () -> str
        """Get the fully qualified C++ type name for an enum."""
        return common.qualify_cpp_name(self._enum.cpp_namespace, self.get_cpp_type_name())

    @abstractmethod
    def get_cpp_type_name(self):
        # type: () -> str
        """Get the C++ type name for an enum."""
        pass

    @abstractmethod
    def get_bson_types(self):
        # type: () -> List[str]
        """Get the BSON type names for an enum."""
        pass

    def _get_enum_deserializer_name(self):
        # type: () -> str
        """Return the name of deserializer function without prefix."""
        return f"{common.title_case(self._enum.name)}_parse"

    def get_enum_deserializer_name(self):
        # type: () -> str
        """Return the name of deserializer function with non-method prefix."""
        return "::" + common.qualify_cpp_name(
            self._enum.cpp_namespace, self._get_enum_deserializer_name()
        )

    def _get_enum_serializer_name(self):
        # type: () -> str
        """Return the name of serializer function without prefix."""
        return f"{common.title_case(self._enum.name)}_serializer"

    def get_enum_serializer_name(self):
        # type: () -> str
        """Return the name of serializer function with non-method prefix."""
        return "::" + common.qualify_cpp_name(
            self._enum.cpp_namespace, self._get_enum_serializer_name()
        )

    def _get_enum_extra_data_name(self):
        # type: () -> str
        """Return the name of the get_extra_data function without prefix."""
        return f"{common.title_case(self._enum.name)}_get_extra_data"

    @abstractmethod
    def get_cpp_value_assignment(self, enum_value):
        # type: (ast.EnumValue) -> str
        """Get the textual representation of the enum value, includes equal sign."""
        pass

    @abstractmethod
    def get_deserializer_declaration(self):
        # type: () -> str
        """Get the deserializer function declaration minus trailing semicolon."""
        pass

    @abstractmethod
    def gen_deserializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the deserializer function definition."""
        pass

    @abstractmethod
    def get_serializer_declaration(self):
        # type: () -> str
        """Get the serializer function declaration minus trailing semicolon."""
        pass

    @abstractmethod
    def gen_serializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the serializer function definition."""
        pass

    def _get_populated_extra_values(self):
        # type: () -> List[Union[syntax.EnumValue,ast.EnumValue]]
        """Filter the enum values to just those containing extra_data."""
        return [val for val in self._enum.values if val.extra_data is not None]

    def get_extra_data_declaration(self):
        # type: () -> Optional[str]
        """Get the get_extra_data function declaration minus trailing semicolon."""
        if len(self._get_populated_extra_values()) == 0:
            return None

        return f"BSONObj {self._get_enum_extra_data_name()}({self.get_cpp_type_name()} value)"

    def gen_extra_data_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the get_extra_data function definition."""

        extra_values = self._get_populated_extra_values()
        if len(extra_values) == 0:
            # No extra data present on this enum.
            return

        # Generate an anonymous namespace full of BSON constants.
        #
        with writer.NamespaceScopeBlock(indented_writer, [""]):
            for enum_value in extra_values:
                indented_writer.write_line("// %s" % json.dumps(enum_value.extra_data))

                bson_value = "".join(
                    [("\\x%02x" % (b)) for b in bson.BSON.encode(enum_value.extra_data)]
                )
                data_name = _get_constant_enum_extra_data_name(self._enum, enum_value)
                indented_writer.write_line(f'const BSONObj {data_name}("{bson_value}");')

        indented_writer.write_empty_line()

        # Generate implementation of get_extra_data function.
        #
        enum_name = self.get_cpp_type_name()
        function_name = self.get_extra_data_declaration()

        with writer.IndentedScopedBlock(indented_writer, f"{function_name} {{", "}"):
            with writer.IndentedScopedBlock(indented_writer, "switch (value) {", "}"):
                for enum_value in extra_values:
                    data_name = _get_constant_enum_extra_data_name(self._enum, enum_value)
                    indented_writer.write_line(
                        f"case {enum_name}::{enum_value.name}: return {data_name};"
                    )
                if len(extra_values) != len(self._enum.values):
                    # One or more enums does not have associated extra data.
                    indented_writer.write_line("default: return BSONObj();")

            if len(extra_values) == len(self._enum.values):
                # All enum cases handled, the compiler should know this.
                indented_writer.write_line("MONGO_UNREACHABLE;")


class _EnumTypeInt(EnumTypeInfoBase, metaclass=ABCMeta):
    """Type information for integer enumerations."""

    def get_cpp_type_name(self):
        # type: () -> str
        return common.title_case(self._enum.name)

    def get_bson_types(self):
        # type: () -> List[str]
        return [self._enum.type]

    def get_cpp_value_assignment(self, enum_value):
        # type: (ast.EnumValue) -> str
        return " = %s" % (enum_value.value)

    def get_deserializer_declaration(self):
        # type: () -> str
        cpp_type = self.get_cpp_type_name()
        deserializer = self._get_enum_deserializer_name()
        return f'{cpp_type} {deserializer}(std::int32_t value, const IDLParserContext& ctxt = IDLParserContext("{self.get_cpp_type_name()}"))'

    def gen_deserializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        enum_values = sorted(cast(ast.Enum, self._enum).values, key=lambda ev: int(ev.value))

        enum_name = self.get_cpp_type_name()
        min_value = enum_values[0].name
        max_value = enum_values[-1].name

        cpp_type = self.get_cpp_type_name()
        func = self._get_enum_deserializer_name()
        indented_writer._stream.write(f"""
{cpp_type} {func}(std::int32_t value, const IDLParserContext& ctxt) {{
    if (!(value >= static_cast<std::underlying_type<{enum_name}>::type>(
        {enum_name}::{min_value}) &&
        value <= static_cast<std::underlying_type<{enum_name}>::type>(
            {enum_name}::{max_value}))) {{
        ctxt.throwBadEnumValue(value);
    }} else {{
        return static_cast<{enum_name}>(value);
    }}
}}""")

    def get_serializer_declaration(self):
        # type: () -> str
        """Get the serializer function declaration minus trailing semicolon."""
        return f"std::int32_t {self._get_enum_serializer_name()}({self.get_cpp_type_name()} value)"

    def gen_serializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the serializer function definition."""

        indented_writer._stream.write(f"""
{self.get_serializer_declaration()} {{
    return static_cast<std::int32_t>(value);
}}""")


def _get_constant_enum_extra_data_name(idl_enum, enum_value):
    # type: (Union[syntax.Enum,ast.Enum], Union[syntax.EnumValue,ast.EnumValue]) -> str
    """Return the C++ name for a string constant of enum extra data value."""
    return f"k{common.title_case(idl_enum.name)}_{enum_value.name}_extra_data"


class _EnumTypeString(EnumTypeInfoBase, metaclass=ABCMeta):
    """Type information for string enumerations."""

    def get_cpp_type_name(self):
        # type: () -> str
        return f"{common.title_case(self._enum.name)}Enum"

    def get_bson_types(self):
        # type: () -> List[str]
        return [self._enum.type]

    def get_cpp_value_assignment(self, enum_value):
        # type: (ast.EnumValue) -> str
        return ""

    def get_deserializer_declaration(self):
        # type: () -> str
        cpp_type = self.get_cpp_type_name()
        func = self._get_enum_deserializer_name()
        return f'{cpp_type} {func}(StringData value, const IDLParserContext& ctxt = IDLParserContext("{cpp_type}"))'

    def gen_deserializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        cpp_type = self.get_cpp_type_name()
        func = self._get_enum_deserializer_name()
        with writer.NamespaceScopeBlock(indented_writer, [""]):
            with writer.IndentedScopedBlock(
                indented_writer, f"constexpr std::array {cpp_type}_values{{", "};"
            ):
                for e in self._enum.values:
                    indented_writer.write_line(f"{cpp_type}::{e.name},")
            with writer.IndentedScopedBlock(
                indented_writer, f"constexpr std::array {cpp_type}_names{{", "};"
            ):
                for e in self._enum.values:
                    indented_writer.write_line(f'"{e.value}"_sd,')
        indented_writer.write_empty_line()

        with writer.IndentedScopedBlock(
            indented_writer,
            f"{cpp_type} {func}(StringData value, const IDLParserContext& ctxt) {{",
            "}",
        ):
            indented_writer.write_line(
                f"static constexpr auto onMatch = [](int i) {{ return {cpp_type}_values[i]; }};"
            )
            indented_writer.write_line(
                f"auto onFail = [&] {{ ctxt.throwBadEnumValue(value); return {cpp_type}{{}}; }};"
            )
            writer.gen_string_table_find_function_block(
                indented_writer,
                "value",
                "onMatch({})",
                "onFail()",
                [e.value for e in self._enum.values],
            )

    def get_serializer_declaration(self):
        # type: () -> str
        """Get the serializer function declaration minus trailing semicolon."""
        cpp_type = self.get_cpp_type_name()
        func = self._get_enum_serializer_name()
        return f"StringData {func}({cpp_type} value)"

    def gen_serializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the serializer function definition."""
        func = self._get_enum_serializer_name()
        cpp_type = self.get_cpp_type_name()
        indented_writer._stream.write(f"""
StringData {func}({cpp_type} value) {{
    auto idx = static_cast<size_t>(value);
    invariant(idx < {cpp_type}_names.size());
    return {cpp_type}_names[idx];
}}
""")


def get_type_info(idl_enum):
    # type: (Union[syntax.Enum,ast.Enum]) -> Optional[EnumTypeInfoBase]
    """Get the type information for a given enumeration type, return None if not supported."""
    if idl_enum.type == "int":
        return _EnumTypeInt(idl_enum)
    elif idl_enum.type == "string":
        return _EnumTypeString(idl_enum)

    return None
