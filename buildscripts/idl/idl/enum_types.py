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
"""
IDL Enum type information.

Support the code generation for enums
"""

from __future__ import absolute_import, print_function, unicode_literals

from abc import ABCMeta, abstractmethod
import textwrap
from typing import cast, List, Optional, Union

from . import ast
from . import common
from . import syntax
from . import writer


class EnumTypeInfoBase(object):
    """Base type for enumeration type information."""

    __metaclass__ = ABCMeta

    def __init__(self, idl_enum):
        # type: (Union[syntax.Enum,ast.Enum]) -> None
        """Construct a EnumTypeInfoBase."""
        self._enum = idl_enum

    @abstractmethod
    def get_cpp_type_name(self):
        # type: () -> unicode
        """Get the C++ type name for an enum."""
        pass

    @abstractmethod
    def get_bson_types(self):
        # type: () -> List[unicode]
        """Get the BSON type names for an enum."""
        pass

    def _get_enum_deserializer_name(self):
        # type: () -> unicode
        """Return the name of deserializer function without prefix."""
        return common.template_args(
            "${enum_name}_parse", enum_name=common.title_case(self._enum.name))

    def get_enum_deserializer_name(self):
        # type: () -> unicode
        """Return the name of deserializer function with non-method prefix."""
        return "::" + self._get_enum_deserializer_name()

    def _get_enum_serializer_name(self):
        # type: () -> unicode
        """Return the name of serializer function without prefix."""
        return common.template_args(
            "${enum_name}_serializer", enum_name=common.title_case(self._enum.name))

    def get_enum_serializer_name(self):
        # type: () -> unicode
        """Return the name of serializer function with non-method prefix."""
        return "::" + self._get_enum_serializer_name()

    @abstractmethod
    def get_cpp_value_assignment(self, enum_value):
        # type: (ast.EnumValue) -> unicode
        """Get the textual representation of the enum value, includes equal sign."""
        pass

    @abstractmethod
    def get_deserializer_declaration(self):
        # type: () -> unicode
        """Get the deserializer function declaration minus trailing semicolon."""
        pass

    @abstractmethod
    def gen_deserializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the deserializer function definition."""
        pass

    @abstractmethod
    def get_serializer_declaration(self):
        # type: () -> unicode
        """Get the serializer function declaration minus trailing semicolon."""
        pass

    @abstractmethod
    def gen_serializer_definition(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the serializer function definition."""
        pass


class _EnumTypeInt(EnumTypeInfoBase):
    """Type information for integer enumerations."""

    __metaclass__ = ABCMeta

    def __init__(self, idl_enum):
        # type: (Union[syntax.Enum,ast.Enum]) -> None
        super(_EnumTypeInt, self).__init__(idl_enum)

    def get_cpp_type_name(self):
        # type: () -> unicode
        return common.title_case(self._enum.name)

    def get_bson_types(self):
        # type: () -> List[unicode]
        return [self._enum.type]

    def get_cpp_value_assignment(self, enum_value):
        # type: (ast.EnumValue) -> unicode
        return " = %s" % (enum_value.value)

    def get_deserializer_declaration(self):
        # type: () -> unicode
        return common.template_args(
            "${enum_name} ${function_name}(const IDLParserErrorContext& ctxt, std::int32_t value)",
            enum_name=self.get_cpp_type_name(),
            function_name=self._get_enum_deserializer_name())

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
        # type: () -> unicode
        """Get the serializer function declaration minus trailing semicolon."""
        return common.template_args(
            "std::int32_t ${function_name}(${enum_name} value)",
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
    # type: (Union[syntax.Enum,ast.Enum], Union[syntax.EnumValue,ast.EnumValue]) -> unicode
    """Return the C++ name for a string constant of string enum value."""
    return common.template_args(
        'k${enum_name}_${name}', enum_name=common.title_case(idl_enum.name), name=enum_value.name)


class _EnumTypeString(EnumTypeInfoBase):
    """Type information for string enumerations."""

    __metaclass__ = ABCMeta

    def __init__(self, idl_enum):
        # type: (Union[syntax.Enum,ast.Enum]) -> None
        super(_EnumTypeString, self).__init__(idl_enum)

    def get_cpp_type_name(self):
        # type: () -> unicode
        return common.template_args(
            "${enum_name}Enum", enum_name=common.title_case(self._enum.name))

    def get_bson_types(self):
        # type: () -> List[unicode]
        return [self._enum.type]

    def get_cpp_value_assignment(self, enum_value):
        # type: (ast.EnumValue) -> unicode
        return ''

    def get_deserializer_declaration(self):
        # type: () -> unicode
        return common.template_args(
            "${enum_name} ${function_name}(const IDLParserErrorContext& ctxt, StringData value)",
            enum_name=self.get_cpp_type_name(),
            function_name=self._get_enum_deserializer_name())

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
                        constant_name=_get_constant_enum_name(self._enum, enum_value),
                        value=enum_value.value))
        indented_writer.write_empty_line()

        with writer.TemplateContext(indented_writer, template_params):
            with writer.IndentedScopedBlock(indented_writer, "${function_name} {", "}"):
                for enum_value in self._enum.values:
                    predicate = 'if (value == %s) {' % (_get_constant_enum_name(self._enum,
                                                                                enum_value))
                    with writer.IndentedScopedBlock(indented_writer, predicate, "}"):
                        indented_writer.write_template('return ${enum_name}::%s;' %
                                                       (enum_value.name))

                indented_writer.write_line("ctxt.throwBadEnumValue(value);")

    def get_serializer_declaration(self):
        # type: () -> unicode
        """Get the serializer function declaration minus trailing semicolon."""
        return common.template_args(
            "StringData ${function_name}(${enum_name} value)",
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
                    with writer.IndentedScopedBlock(indented_writer,
                                                    'if (value == ${enum_name}::%s) {' %
                                                    (enum_value.name), "}"):
                        indented_writer.write_line('return %s;' % (_get_constant_enum_name(
                            self._enum, enum_value)))

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
