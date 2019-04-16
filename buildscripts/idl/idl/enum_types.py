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

from abc import ABCMeta, abstractmethod
import textwrap
from typing import cast, List, Optional, Union

from . import ast
from . import common
from . import syntax
from . import writer


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
        return common.template_args("${enum_name}_parse",
                                    enum_name=common.title_case(self._enum.name))

    def get_enum_deserializer_name(self):
        # type: () -> str
        """Return the name of deserializer function with non-method prefix."""
        return "::" + common.qualify_cpp_name(self._enum.cpp_namespace,
                                              self._get_enum_deserializer_name())

    def _get_enum_serializer_name(self):
        # type: () -> str
        """Return the name of serializer function without prefix."""
        return common.template_args("${enum_name}_serializer",
                                    enum_name=common.title_case(self._enum.name))

    def get_enum_serializer_name(self):
        # type: () -> str
        """Return the name of serializer function with non-method prefix."""
        return "::" + common.qualify_cpp_name(self._enum.cpp_namespace,
                                              self._get_enum_serializer_name())

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
        return common.template_args(
            "${enum_name} ${function_name}(const IDLParserErrorContext& ctxt, std::int32_t value)",
            enum_name=self.get_cpp_type_name(), function_name=self._get_enum_deserializer_name())

    def gen_deserializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        enum_values = sorted(cast(ast.Enum, self._enum).values, key=lambda ev: int(ev.value))

        template_params = {
            'enum_name': self.get_cpp_type_name(),
            'function_name': self.get_deserializer_declaration(),
            'min_value': enum_values[0].name,
            'max_value': enum_values[-1].name,
        }

        with writer.TemplateContext(indented_writer, template_params):
            with writer.IndentedScopedBlock(indented_writer, "${function_name} {", "}"):
                indented_writer.write_template(
                    textwrap.dedent("""
            if (!(value >= static_cast<std::underlying_type<${enum_name}>::type>(
                ${enum_name}::${min_value}) &&
                value <= static_cast<std::underlying_type<${enum_name}>::type>(
                    ${enum_name}::${max_value}))) {
                ctxt.throwBadEnumValue(value);
            } else {
                return static_cast<${enum_name}>(value);
            }
                """))

    def get_serializer_declaration(self):
        # type: () -> str
        """Get the serializer function declaration minus trailing semicolon."""
        return common.template_args("std::int32_t ${function_name}(${enum_name} value)",
                                    enum_name=self.get_cpp_type_name(),
                                    function_name=self._get_enum_serializer_name())

    def gen_serializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the serializer function definition."""
        template_params = {
            'enum_name': self.get_cpp_type_name(),
            'function_name': self.get_serializer_declaration(),
        }

        with writer.TemplateContext(indented_writer, template_params):
            with writer.IndentedScopedBlock(indented_writer, "${function_name} {", "}"):
                indented_writer.write_template('return static_cast<std::int32_t>(value);')


def _get_constant_enum_name(idl_enum, enum_value):
    # type: (Union[syntax.Enum,ast.Enum], Union[syntax.EnumValue,ast.EnumValue]) -> str
    """Return the C++ name for a string constant of string enum value."""
    return common.template_args('k${enum_name}_${name}', enum_name=common.title_case(idl_enum.name),
                                name=enum_value.name)


class _EnumTypeString(EnumTypeInfoBase, metaclass=ABCMeta):
    """Type information for string enumerations."""

    def get_cpp_type_name(self):
        # type: () -> str
        return common.template_args("${enum_name}Enum",
                                    enum_name=common.title_case(self._enum.name))

    def get_bson_types(self):
        # type: () -> List[str]
        return [self._enum.type]

    def get_cpp_value_assignment(self, enum_value):
        # type: (ast.EnumValue) -> str
        return ''

    def get_deserializer_declaration(self):
        # type: () -> str
        return common.template_args(
            "${enum_name} ${function_name}(const IDLParserErrorContext& ctxt, StringData value)",
            enum_name=self.get_cpp_type_name(), function_name=self._get_enum_deserializer_name())

    def gen_deserializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        template_params = {
            'enum_name': self.get_cpp_type_name(),
            'function_name': self.get_deserializer_declaration(),
        }

        # Generate an anonymous namespace full of string constants
        #
        with writer.NamespaceScopeBlock(indented_writer, ['']):
            for enum_value in self._enum.values:
                indented_writer.write_line(
                    common.template_args(
                        'constexpr StringData ${constant_name} = "${value}"_sd;',
                        constant_name=_get_constant_enum_name(self._enum,
                                                              enum_value), value=enum_value.value))
        indented_writer.write_empty_line()

        with writer.TemplateContext(indented_writer, template_params):
            with writer.IndentedScopedBlock(indented_writer, "${function_name} {", "}"):
                for enum_value in self._enum.values:
                    predicate = 'if (value == %s) {' % (_get_constant_enum_name(
                        self._enum, enum_value))
                    with writer.IndentedScopedBlock(indented_writer, predicate, "}"):
                        indented_writer.write_template(
                            'return ${enum_name}::%s;' % (enum_value.name))

                indented_writer.write_line("ctxt.throwBadEnumValue(value);")

    def get_serializer_declaration(self):
        # type: () -> str
        """Get the serializer function declaration minus trailing semicolon."""
        return common.template_args("StringData ${function_name}(${enum_name} value)",
                                    enum_name=self.get_cpp_type_name(),
                                    function_name=self._get_enum_serializer_name())

    def gen_serializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the serializer function definition."""
        template_params = {
            'enum_name': self.get_cpp_type_name(),
            'function_name': self.get_serializer_declaration(),
        }

        with writer.TemplateContext(indented_writer, template_params):
            with writer.IndentedScopedBlock(indented_writer, "${function_name} {", "}"):
                for enum_value in self._enum.values:
                    with writer.IndentedScopedBlock(
                            indented_writer, 'if (value == ${enum_name}::%s) {' % (enum_value.name),
                            "}"):
                        indented_writer.write_line(
                            'return %s;' % (_get_constant_enum_name(self._enum, enum_value)))

                indented_writer.write_line('MONGO_UNREACHABLE;')
                indented_writer.write_line('return StringData();')


def get_type_info(idl_enum):
    # type: (Union[syntax.Enum,ast.Enum]) -> Optional[EnumTypeInfoBase]
    """Get the type information for a given enumeration type, return None if not supported."""
    if idl_enum.type == 'int':
        return _EnumTypeInt(idl_enum)
    elif idl_enum.type == 'string':
        return _EnumTypeString(idl_enum)

    return None
