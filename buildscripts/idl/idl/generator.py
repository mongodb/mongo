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
# pylint: disable=too-many-lines
"""IDL C++ Code Generator."""

from __future__ import absolute_import, print_function, unicode_literals

from abc import ABCMeta, abstractmethod
import io
import os
import string
import sys
import textwrap
from typing import cast, List, Mapping, Union

from . import ast
from . import bson
from . import common
from . import cpp_types
from . import enum_types
from . import struct_types
from . import writer


def _get_field_member_name(field):
    # type: (ast.Field) -> unicode
    """Get the C++ class member name for a field."""
    return '_%s' % (common.camel_case(field.cpp_name))


def _get_has_field_member_name(field):
    # type: (ast.Field) -> unicode
    """Get the C++ class member name for bool 'has' member field."""
    return '_has%s' % (common.title_case(field.cpp_name))


def _is_required_serializer_field(field):
    # type: (ast.Field) -> bool
    """
    Get whether we require this field to have a value set before serialization.

    Fields that must be set before serialization are fields without default values, that are not
    optional, and are not chained.
    """
    return not field.ignore and not field.optional and not field.default and not field.chained


def _get_field_constant_name(field):
    # type: (ast.Field) -> unicode
    """Get the C++ string constant name for a field."""
    return common.template_args(
        'k${constant_name}FieldName', constant_name=common.title_case(field.cpp_name))


def _access_member(field):
    # type: (ast.Field) -> unicode
    """Get the declaration to access a member for a field."""
    member_name = _get_field_member_name(field)

    if not field.optional:
        return '%s' % (member_name)

    # optional types need a method call to access their values
    return '%s.get()' % (member_name)


def _get_bson_type_check(bson_element, ctxt_name, field):
    # type: (unicode, unicode, ast.Field) -> unicode
    """Get the C++ bson type check for a field."""
    bson_types = field.bson_serialization_type
    if len(bson_types) == 1:
        if bson_types[0] in ['any', 'chain']:
            # Skip BSON validation for 'any' types since they are required to validate the
            # BSONElement.
            # Skip BSON validation for 'chain' types since they process the raw BSONObject the
            # encapsulating IDL struct parser is passed.
            return None

        if not bson_types[0] == 'bindata':
            return '%s.checkAndAssertType(%s, %s)' % (ctxt_name, bson_element,
                                                      bson.cpp_bson_type_name(bson_types[0]))
        else:
            return '%s.checkAndAssertBinDataType(%s, %s)' % (
                ctxt_name, bson_element, bson.cpp_bindata_subtype_type_name(field.bindata_subtype))
    else:
        type_list = '{%s}' % (', '.join([bson.cpp_bson_type_name(b) for b in bson_types]))
        return '%s.checkAndAssertTypes(%s, %s)' % (ctxt_name, bson_element, type_list)


class _FieldUsageCheckerBase(object):
    """Check for duplicate fields, and required fields as needed."""

    __metaclass__ = ABCMeta

    def __init__(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Create a field usage checker."""
        self._writer = indented_writer  # type: writer.IndentedTextWriter
        self._fields = []  # type: List[ast.Field]

    @abstractmethod
    def add_store(self, field_name):
        # type: (unicode) -> None
        """Create the C++ field store initialization code."""
        pass

    @abstractmethod
    def add(self, field, bson_element_variable):
        # type: (ast.Field, unicode) -> None
        """Add a field to track."""
        pass

    @abstractmethod
    def add_final_checks(self):
        # type: () -> None
        """Output the code to check for missing fields."""
        pass


class _SlowFieldUsageChecker(_FieldUsageCheckerBase):
    """
    Check for duplicate fields, and required fields as needed.

    Detects duplicate extra fields.
    Generates code with a C++ std::set to maintain a set of fields seen while parsing a BSON
    document. The std::set has O(N lg N) lookup, and allocates memory in the heap.
    """

    def __init__(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        super(_SlowFieldUsageChecker, self).__init__(indented_writer)

        self._writer.write_line('std::set<StringData> usedFields;')

    def add_store(self, field_name):
        # type: (unicode) -> None
        self._writer.write_line('auto push_result = usedFields.insert(%s);' % (field_name))
        with writer.IndentedScopedBlock(self._writer, 'if (push_result.second == false) {', '}'):
            self._writer.write_line('ctxt.throwDuplicateField(%s);' % (field_name))

    def add(self, field, bson_element_variable):
        # type: (ast.Field, unicode) -> None
        if not field in self._fields:
            self._fields.append(field)

    def add_final_checks(self):
        # type: () -> None
        for field in self._fields:
            if (not field.optional) and (not field.ignore) and (not field.chained):
                with writer.IndentedScopedBlock(self._writer,
                                                'if (usedFields.find(%s) == usedFields.end()) {' %
                                                (_get_field_constant_name(field)), '}'):
                    if field.default:
                        self._writer.write_line('%s = %s;' %
                                                (_get_field_member_name(field), field.default))
                    else:
                        self._writer.write_line('ctxt.throwMissingField(%s);' %
                                                (_get_field_constant_name(field)))


def _gen_field_usage_constant(field):
    # type: (ast.Field) -> unicode
    """Get the name for a bitset constant in field usage checking."""
    return "k%sBit" % (common.title_case(field.cpp_name))


class _FastFieldUsageChecker(_FieldUsageCheckerBase):
    """
    Check for duplicate fields, and required fields as needed.

    Does not detect duplicate extra fields. Only works for strict parsers.
    Generates code with a C++ std::bitset to maintain a record each field seen while parsing a
    document. The std::bitset has O(1) lookup, and allocates a single int or similar on the stack.
    """

    def __init__(self, indented_writer, fields):
        # type: (writer.IndentedTextWriter, List[ast.Field]) -> None
        super(_FastFieldUsageChecker, self).__init__(indented_writer)

        self._writer.write_line('std::bitset<%d> usedFields;' % (len(fields)))

        bit_id = 0
        for field in fields:
            if field.chained:
                continue

            self._writer.write_line('const size_t %s = %d;' %
                                    (_gen_field_usage_constant(field), bit_id))
            bit_id += 1

    def add_store(self, field_name):
        # type: (unicode) -> None
        """Create the C++ field store initialization code."""
        pass

    def add(self, field, bson_element_variable):
        # type: (ast.Field, unicode) -> None
        """Add a field to track."""
        if not field in self._fields:
            self._fields.append(field)

        with writer.IndentedScopedBlock(self._writer, 'if (usedFields[%s]) {' %
                                        (_gen_field_usage_constant(field)), '}'):
            self._writer.write_line('ctxt.throwDuplicateField(%s);' % (bson_element_variable))
        self._writer.write_empty_line()

        self._writer.write_line('usedFields.set(%s);' % (_gen_field_usage_constant(field)))
        self._writer.write_empty_line()

    def add_final_checks(self):
        # type: () -> None
        """Output the code to check for missing fields."""
        with writer.IndentedScopedBlock(self._writer, 'if (!usedFields.all()) {', '}'):
            for field in self._fields:
                if (not field.optional) and (not field.ignore):
                    with writer.IndentedScopedBlock(self._writer, 'if (!usedFields[%s]) {' %
                                                    (_gen_field_usage_constant(field)), '}'):
                        if field.default:
                            self._writer.write_line('%s = %s;' %
                                                    (_get_field_member_name(field), field.default))
                        else:
                            self._writer.write_line('ctxt.throwMissingField(%s);' %
                                                    (_get_field_constant_name(field)))


def _get_field_usage_checker(indented_writer, struct):
    # type: (writer.IndentedTextWriter, ast.Struct) -> _FieldUsageCheckerBase

    # Only use the fast field usage checker if we never expect extra fields that we need to ignore
    # but still wish to do duplicate detection on.
    if struct.strict:
        return _FastFieldUsageChecker(indented_writer, struct.fields)

    return _SlowFieldUsageChecker(indented_writer)


class _CppFileWriterBase(object):
    """
    C++ File writer.

    Encapsulates low level knowledge of how to print a C++ file.
    Relies on caller to orchestrate calls correctly though.
    """

    def __init__(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Create a C++ code writer."""
        self._writer = indented_writer  # type: writer.IndentedTextWriter

    def write_unindented_line(self, msg):
        # type: (unicode) -> None
        """Write an unindented line to the stream."""
        self._writer.write_unindented_line(msg)

    def write_empty_line(self):
        # type: () -> None
        """Write an empty line to the stream."""
        self._writer.write_empty_line()

    def gen_file_header(self):
        # type: () -> None
        """Generate a file header saying the file is generated."""
        self._writer.write_unindented_line(
            textwrap.dedent("""\
        /**
         * WARNING: This is a generated file. Do not modify.
         *
         * Source: %s
         */
            """ % (" ".join(sys.argv))))

    def gen_system_include(self, include):
        # type: (unicode) -> None
        """Generate a system C++ include line."""
        self._writer.write_unindented_line('#include <%s>' % (include))

    def gen_include(self, include):
        # type: (unicode) -> None
        """Generate a non-system C++ include line."""
        self._writer.write_unindented_line('#include "%s"' % (include))

    def gen_namespace_block(self, namespace):
        # type: (unicode) -> writer.NamespaceScopeBlock
        """Generate a namespace block."""
        namespace_list = namespace.split("::")

        return writer.NamespaceScopeBlock(self._writer, namespace_list)

    def gen_description_comment(self, description):
        # type: (unicode) -> None
        """Generate a multiline comment with the description from the IDL."""
        self._writer.write_line(
            textwrap.dedent("""\
        /**
         * %s
         */""" % (description)))

    def _with_template(self, template_params):
        # type: (Mapping[unicode,unicode]) -> writer.TemplateContext
        """Generate a template context for the current parameters."""
        return writer.TemplateContext(self._writer, template_params)

    def _block(self, opening, closing):
        # type: (unicode, unicode) -> Union[writer.IndentedScopedBlock,writer.EmptyBlock]
        """Generate an indented block if opening is not empty."""
        if not opening:
            return writer.EmptyBlock()

        return writer.IndentedScopedBlock(self._writer, opening, closing)

    def _predicate(self, check_str, use_else_if=False):
        # type: (unicode, bool) -> Union[writer.IndentedScopedBlock,writer.EmptyBlock]
        """
        Generate an if block if the condition is not-empty.

        Generate 'else if' instead of use_else_if is True.
        """
        if not check_str:
            return writer.EmptyBlock()

        conditional = 'if'
        if use_else_if:
            conditional = 'else if'

        return writer.IndentedScopedBlock(self._writer, '%s (%s) {' % (conditional, check_str), '}')


class _CppHeaderFileWriter(_CppFileWriterBase):
    """C++ .h File writer."""

    def __init__(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Create a C++ .cpp file code writer."""
        super(_CppHeaderFileWriter, self).__init__(indented_writer)

    def gen_class_declaration_block(self, class_name):
        # type: (unicode) -> writer.IndentedScopedBlock
        """Generate a class declaration block."""
        return writer.IndentedScopedBlock(self._writer,
                                          'class %s {' % common.title_case(class_name), '};')

    def gen_class_constructors(self, struct):
        # type: (ast.Struct) -> None
        """Generate the declarations for the class constructors."""
        struct_type_info = struct_types.get_struct_info(struct)

        self._writer.write_line(struct_type_info.get_constructor_method().get_declaration())

    def gen_serializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate a serializer method declarations."""

        struct_type_info = struct_types.get_struct_info(struct)

        parse_method = struct_type_info.get_deserializer_static_method()
        if parse_method:
            self._writer.write_line(parse_method.get_declaration())

        parse_method = struct_type_info.get_op_msg_request_deserializer_static_method()
        if parse_method:
            self._writer.write_line(parse_method.get_declaration())

        self._writer.write_line(struct_type_info.get_serializer_method().get_declaration())

        parse_method = struct_type_info.get_op_msg_request_serializer_method()
        if parse_method:
            self._writer.write_line(parse_method.get_declaration())

        self._writer.write_line(struct_type_info.get_to_bson_method().get_declaration())

        self._writer.write_empty_line()

    def gen_protected_serializer_methods(self, struct):
        # type: (ast.Struct) -> None
        # pylint: disable=invalid-name
        """Generate protected serializer method declarations."""
        struct_type_info = struct_types.get_struct_info(struct)

        parse_method = struct_type_info.get_deserializer_method()
        if parse_method:
            self._writer.write_line(parse_method.get_declaration())

        parse_method = struct_type_info.get_op_msg_request_deserializer_method()
        if parse_method:
            self._writer.write_line(parse_method.get_declaration())

        self._writer.write_empty_line()

    def gen_getter(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ getter definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        param_type = cpp_type_info.get_getter_setter_type()
        member_name = _get_field_member_name(field)

        if cpp_type_info.return_by_reference():
            param_type += "&"

        template_params = {
            'method_name': common.title_case(field.cpp_name),
            'param_type': param_type,
            'body': cpp_type_info.get_getter_body(member_name),
            'const_type': 'const ' if cpp_type_info.is_const_type() else '',
        }

        # Generate a getter that disables xvalue for view types (i.e. StringData), constructed
        # optional types, and non-primitive types.
        with self._with_template(template_params):

            if cpp_type_info.disable_xvalue():
                self._writer.write_template(
                    'const ${param_type} get${method_name}() const& { ${body} }')
                self._writer.write_template('void get${method_name}() && = delete;')
            else:
                self._writer.write_template(
                    '${const_type}${param_type} get${method_name}() const { ${body} }')

    def gen_setter(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ setter definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        param_type = cpp_type_info.get_getter_setter_type()
        member_name = _get_field_member_name(field)

        post_body = ''
        if _is_required_serializer_field(field):
            post_body = '%s = true;' % (_get_has_field_member_name(field))

        template_params = {
            'method_name': common.title_case(field.cpp_name),
            'member_name': member_name,
            'param_type': param_type,
            'body': cpp_type_info.get_setter_body(member_name),
            'post_body': post_body,
        }

        with self._with_template(template_params):
            self._writer.write_template('void set${method_name}(${param_type} value) & ' +
                                        '{ ${body} ${post_body} }')

        self._writer.write_empty_line()

    def gen_member(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ class member definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        member_type = cpp_type_info.get_storage_type()
        member_name = _get_field_member_name(field)

        if field.default and not field.constructed:
            self._writer.write_line('%s %s{%s};' % (member_type, member_name, field.default))
        else:
            self._writer.write_line('%s %s;' % (member_type, member_name))

    def gen_serializer_member(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ class bool has_<field> member definition for a field."""
        has_member_name = _get_has_field_member_name(field)

        # Use a bitfield to save space
        self._writer.write_line('bool %s : 1;' % (has_member_name))

    def gen_string_constants_declarations(self, struct):
        # type: (ast.Struct) -> None
        # pylint: disable=invalid-name
        """Generate a StringData constant for field name."""
        sorted_fields = sorted([field for field in struct.fields], key=lambda f: f.cpp_name)

        for field in sorted_fields:
            self._writer.write_line(
                common.template_args(
                    'static constexpr auto ${constant_name} = "${field_name}"_sd;',
                    constant_name=_get_field_constant_name(field),
                    field_name=field.name))

        if isinstance(struct, ast.Command):
            self._writer.write_line(
                common.template_args(
                    'static constexpr auto kCommandName = "${struct_name}"_sd;',
                    struct_name=struct.name))

    def gen_enum_functions(self, idl_enum):
        # type: (ast.Enum) -> None
        """Generate the declaration for an enum's supporting functions."""
        enum_type_info = enum_types.get_type_info(idl_enum)

        self._writer.write_line("%s;" % (enum_type_info.get_deserializer_declaration()))

        self._writer.write_line("%s;" % (enum_type_info.get_serializer_declaration()))

    def gen_enum_declaration(self, idl_enum):
        # type: (ast.Enum) -> None
        """Generate the declaration for an enum."""
        enum_type_info = enum_types.get_type_info(idl_enum)

        with self._block('enum class %s : std::int32_t {' % (enum_type_info.get_cpp_type_name()),
                         '};'):
            for enum_value in idl_enum.values:
                self._writer.write_line(
                    common.template_args(
                        '${name} ${value},',
                        name=enum_value.name,
                        value=enum_type_info.get_cpp_value_assignment(enum_value)))

    def gen_op_msg_request_methods(self, command):
        # type: (ast.Command) -> None
        """Generate the methods for a command."""
        struct_type_info = struct_types.get_struct_info(command)
        struct_type_info.gen_getter_method(self._writer)

        self._writer.write_empty_line()

    def gen_op_msg_request_member(self, command):
        # type: (ast.Command) -> None
        """Generate the class members for a command."""
        struct_type_info = struct_types.get_struct_info(command)
        struct_type_info.gen_member(self._writer)

        self._writer.write_empty_line()

    def gen_known_fields_declaration(self):
        # type: () -> None
        """Generate a known fields vector for a command."""
        self._writer.write_line("static const std::vector<StringData> _knownFields;")
        self.write_empty_line()

    def generate(self, spec):
        # type: (ast.IDLAST) -> None
        """Generate the C++ header to a stream."""
        # pylint: disable=too-many-branches
        self.gen_file_header()

        self._writer.write_unindented_line('#pragma once')
        self.write_empty_line()

        # Generate system includes first
        header_list = [
            'algorithm',
            'boost/optional.hpp',
            'cstdint',
            'string',
            'vector',
        ]

        header_list.sort()

        for include in header_list:
            self.gen_system_include(include)

        self.write_empty_line()

        # Generate user includes second
        header_list = [
            'mongo/base/string_data.h',
            'mongo/base/data_range.h',
            'mongo/bson/bsonobj.h',
            'mongo/bson/bsonobjbuilder.h',
            'mongo/idl/idl_parser.h',
            'mongo/util/net/op_msg.h',
        ] + spec.globals.cpp_includes

        header_list.sort()

        for include in header_list:
            self.gen_include(include)

        self.write_empty_line()

        # Generate namesapce
        with self.gen_namespace_block(spec.globals.cpp_namespace):
            self.write_empty_line()

            for idl_enum in spec.enums:
                self.gen_description_comment(idl_enum.description)
                self.gen_enum_declaration(idl_enum)
                self._writer.write_empty_line()

                self.gen_enum_functions(idl_enum)
                self._writer.write_empty_line()

            spec_and_structs = spec.structs
            spec_and_structs += spec.commands

            for struct in spec_and_structs:
                self.gen_description_comment(struct.description)
                with self.gen_class_declaration_block(struct.name):
                    self.write_unindented_line('public:')

                    # Generate a sorted list of string constants
                    self.gen_string_constants_declarations(struct)
                    self.write_empty_line()

                    # Write constructor
                    self.gen_class_constructors(struct)
                    self.write_empty_line()

                    # Write serialization
                    self.gen_serializer_methods(struct)

                    if isinstance(struct, ast.Command):
                        self.gen_op_msg_request_methods(struct)

                    # Write getters & setters
                    for field in struct.fields:
                        if not field.ignore:
                            if field.description:
                                self.gen_description_comment(field.description)
                            self.gen_getter(field)
                            self.gen_setter(field)

                    self.write_unindented_line('protected:')
                    self.gen_protected_serializer_methods(struct)

                    self.write_unindented_line('private:')

                    # Write command member variables
                    if isinstance(struct, ast.Command):
                        self.gen_known_fields_declaration()
                        self.write_empty_line()

                        self.gen_op_msg_request_member(struct)

                    # Write member variables
                    for field in struct.fields:
                        if not field.ignore:
                            self.gen_member(field)

                    # Write serializer member variables
                    # Note: we write these out second to ensure the bit fields can be packed by
                    # the compiler.
                    for field in struct.fields:
                        if _is_required_serializer_field(field):
                            self.gen_serializer_member(field)

                self.write_empty_line()


class _CppSourceFileWriter(_CppFileWriterBase):
    """C++ .cpp File writer."""

    def __init__(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Create a C++ .cpp file code writer."""
        super(_CppSourceFileWriter, self).__init__(indented_writer)

    def _gen_field_deserializer_expression(self, element_name, field):
        # type: (unicode, ast.Field) -> unicode
        # pylint: disable=invalid-name
        """
        Generate the C++ deserializer piece for a field.

        Writes multiple lines into the generated file.
        Returns the final statement to access the deserialized value.
        """

        if field.struct_type:
            self._writer.write_line('IDLParserErrorContext tempContext(%s, &ctxt);' %
                                    (_get_field_constant_name(field)))
            self._writer.write_line('const auto localObject = %s.Obj();' % (element_name))
            return '%s::parse(tempContext, localObject)' % (common.title_case(field.struct_type))
        elif field.deserializer and 'BSONElement::' in field.deserializer:
            method_name = writer.get_method_name(field.deserializer)
            return '%s.%s()' % (element_name, method_name)
        else:
            # Custom method, call the method on object.
            bson_cpp_type = cpp_types.get_bson_cpp_type(field)

            if bson_cpp_type:
                # Call a static class method with the signature:
                # Class Class::method(StringData value)
                # or
                # Class::method(const BSONObj& value)
                expression = bson_cpp_type.gen_deserializer_expression(self._writer, element_name)
                if field.deserializer:
                    method_name = writer.get_method_name_from_qualified_method_name(
                        field.deserializer)

                    # For fields which are enums, pass a IDLParserErrorContext
                    if field.enum_type:
                        self._writer.write_line('IDLParserErrorContext tempContext(%s, &ctxt);' %
                                                (_get_field_constant_name(field)))
                        return common.template_args(
                            "${method_name}(tempContext, ${expression})",
                            method_name=method_name,
                            expression=expression)
                    else:
                        return common.template_args(
                            "${method_name}(${expression})",
                            method_name=method_name,
                            expression=expression)
                else:
                    # BSONObjects are allowed to be pass through without deserialization
                    assert field.bson_serialization_type == ['object']
                    return expression
            else:
                # Call a static class method with the signature:
                # Class Class::method(const BSONElement& value)
                method_name = writer.get_method_name_from_qualified_method_name(field.deserializer)

                return '%s(%s)' % (method_name, element_name)

    def _gen_array_deserializer(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ deserializer piece for an array field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        cpp_type = cpp_type_info.get_type_name()

        self._writer.write_line('std::uint32_t expectedFieldNumber{0};')
        self._writer.write_line('const IDLParserErrorContext arrayCtxt(%s, &ctxt);' %
                                (_get_field_constant_name(field)))
        self._writer.write_line('std::vector<%s> values;' % (cpp_type))
        self._writer.write_empty_line()

        self._writer.write_line('const BSONObj arrayObject = element.Obj();')

        with self._block('for (const auto& arrayElement : arrayObject) {', '}'):

            self._writer.write_line(
                'const auto arrayFieldName = arrayElement.fieldNameStringData();')
            self._writer.write_line('std::uint32_t fieldNumber;')
            self._writer.write_empty_line()

            # Check the array field names are integers
            self._writer.write_line(
                'Status status = parseNumberFromString(arrayFieldName, &fieldNumber);')
            with self._predicate('status.isOK()'):

                # Check that the array field names are sequential
                with self._predicate('fieldNumber != expectedFieldNumber'):
                    self._writer.write_line('arrayCtxt.throwBadArrayFieldNumberSequence(' +
                                            'fieldNumber, expectedFieldNumber);')
                self._writer.write_empty_line()

                with self._predicate(_get_bson_type_check('arrayElement', 'arrayCtxt', field)):
                    array_value = self._gen_field_deserializer_expression('arrayElement', field)
                    self._writer.write_line('values.emplace_back(%s);' % (array_value))

            with self._block('else {', '}'):
                self._writer.write_line('arrayCtxt.throwBadArrayFieldNumberValue(arrayFieldName);')

            self._writer.write_line('++expectedFieldNumber;')

        self._writer.write_line('%s = std::move(values);' % (_get_field_member_name(field)))

    def gen_field_deserializer(self, field, bson_object):
        # type: (ast.Field, unicode) -> None
        """Generate the C++ deserializer piece for a field."""
        if field.array:
            self._gen_array_deserializer(field)
            return

        if field.chained:
            # Do not generate a predicate check since we always call these deserializers.

            if field.struct_type:
                # Do not generate a new parser context, reuse the current one since we are not
                # entering a nested document.
                expression = '%s::parse(ctxt, %s)' % (common.title_case(field.struct_type),
                                                      bson_object)
            else:
                method_name = writer.get_method_name_from_qualified_method_name(field.deserializer)
                expression = "%s(%s)" % (method_name, bson_object)

            self._writer.write_line('%s = %s;' % (_get_field_member_name(field), expression))
        else:
            # May be an empty block if the type is 'any'
            with self._predicate(_get_bson_type_check('element', 'ctxt', field)):
                object_value = self._gen_field_deserializer_expression('element', field)
                self._writer.write_line('%s = %s;' % (_get_field_member_name(field), object_value))

    def gen_doc_sequence_deserializer(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ deserializer piece for a C++ mongo::OpMsg::DocumentSequence."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        cpp_type = cpp_type_info.get_type_name()

        self._writer.write_line('std::vector<%s> values;' % (cpp_type))
        self._writer.write_empty_line()

        # TODO: add support for sequence length checks, today we allow an empty document sequence
        # because we do not give a way for IDL specifications to specify if they allow empty
        # sequences or require non-empty sequences.

        with self._block('for (const BSONObj& sequenceObject : sequence.objs) {', '}'):

            # Either we are deserializing BSON Objects or IDL structs
            if field.struct_type:
                self._writer.write_line('IDLParserErrorContext tempContext(%s, &ctxt);' %
                                        (_get_field_constant_name(field)))
                array_value = '%s::parse(tempContext, sequenceObject)' % (
                    common.title_case(field.struct_type))
            else:
                assert field.bson_serialization_type == ['object']
                array_value = "sequenceObject"

            self._writer.write_line('values.emplace_back(%s);' % (array_value))

        self._writer.write_line('%s = std::move(values);' % (_get_field_member_name(field)))

    def gen_op_msg_request_namespace_check(self, struct):
        # type: (ast.Struct) -> None
        """Generate a namespace check for a command."""
        # pylint: disable=invalid-name
        if not isinstance(struct, ast.Command):
            return

        with self._predicate("firstFieldFound == false"):
            # Get the Command element if we need it for later in the deserializer to get the
            # namespace
            if struct.namespace != common.COMMAND_NAMESPACE_IGNORED:
                self._writer.write_line('commandElement = element;')

            self._writer.write_line('firstFieldFound = true;')
            self._writer.write_line('continue;')

    def gen_constructors(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ constructor definition."""

        struct_type_info = struct_types.get_struct_info(struct)
        constructor = struct_type_info.get_constructor_method()

        initializers = ['_%s(std::move(%s))' % (arg.name, arg.name) for arg in constructor.args]

        initializes_db_name = False
        if [arg for arg in constructor.args if arg.name == 'nss']:
            initializers.append('_dbName(nss.db().toString())')
            initializes_db_name = True

        initializers += [
            '%s(false)' % _get_has_field_member_name(field) for field in struct.fields
            if _is_required_serializer_field(field) and not (field.name == "$db" and
                                                             initializes_db_name)
        ]

        if initializes_db_name:
            initializers.append('_hasDbName(true)')

        initializers_str = ''
        if initializers:
            initializers_str = ': ' + ', '.join(initializers)

        with self._block('%s %s {' % (constructor.get_definition(), initializers_str), '}'):
            self._writer.write_line('// Used for initialization only')

    def _gen_fields_deserializer_common(self, struct, bson_object):
        # type: (ast.Struct, unicode) -> _FieldUsageCheckerBase
        """Generate the C++ code to deserialize list of fields."""
        # pylint: disable=too-many-branches
        field_usage_check = _get_field_usage_checker(self._writer, struct)
        if isinstance(struct, ast.Command):
            self._writer.write_line('BSONElement commandElement;')
            self._writer.write_line('bool firstFieldFound = false;')

        self._writer.write_empty_line()

        with self._block('for (const auto& element :%s) {' % (bson_object), '}'):

            self._writer.write_line('const auto fieldName = element.fieldNameStringData();')
            self._writer.write_empty_line()

            if isinstance(struct, ast.Command):
                with self._predicate("firstFieldFound == false"):
                    # Get the Command element if we need it for later in the deserializer to get the
                    # namespace
                    if struct.namespace != common.COMMAND_NAMESPACE_IGNORED:
                        self._writer.write_line('commandElement = element;')

                    self._writer.write_line('firstFieldFound = true;')
                    self._writer.write_line('continue;')

            field_usage_check.add_store("fieldName")
            self._writer.write_empty_line()

            first_field = True
            for field in struct.fields:
                # Do not parse chained fields as fields since they are actually chained types.
                if field.chained:
                    continue

                field_predicate = 'fieldName == %s' % (_get_field_constant_name(field))

                with self._predicate(field_predicate, not first_field):
                    field_usage_check.add(field, "element")

                    if field.ignore:
                        self._writer.write_line('// ignore field')
                    else:
                        if _is_required_serializer_field(field):
                            self._writer.write_line('%s = true;' %
                                                    (_get_has_field_member_name(field)))

                        self.gen_field_deserializer(field, bson_object)

                if first_field:
                    first_field = False

            # End of for fields
            # Generate strict check for extranous fields
            if struct.strict:
                with self._block('else {', '}'):
                    # For commands, check if this a well known command field that the IDL parser
                    # should ignore regardless of strict mode.
                    command_predicate = None
                    if isinstance(struct, ast.Command):
                        command_predicate = "!Command::isGenericArgument(fieldName)"

                    with self._predicate(command_predicate):
                        self._writer.write_line('ctxt.throwUnknownField(fieldName);')

        # Parse chained types
        for field in struct.fields:
            if not field.chained:
                continue

            # Simply generate deserializers since these are all 'any' types
            self.gen_field_deserializer(field, bson_object)
        self._writer.write_empty_line()

        self._writer.write_empty_line()

        return field_usage_check

    def gen_bson_deserializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ deserializer method definitions."""
        struct_type_info = struct_types.get_struct_info(struct)

        if not struct_type_info.get_deserializer_static_method():
            return
        assert not isinstance(struct, ast.Command)

        with self._block('%s {' %
                         (struct_type_info.get_deserializer_static_method().get_definition()), '}'):
            self._writer.write_line('%s object;' % common.title_case(struct.name))
            self._writer.write_line(struct_type_info.get_deserializer_method().get_call('object'))
            self._writer.write_line('return object;')

        func_def = struct_type_info.get_deserializer_method().get_definition()
        with self._block('%s {' % (func_def), '}'):

            # Deserialize all the fields
            field_usage_check = self._gen_fields_deserializer_common(struct, "bsonObject")

            # Check for required fields
            field_usage_check.add_final_checks()
            self._writer.write_empty_line()

            # Generate namespace check now that "$db" has been read or defaulted
            struct_type_info.gen_namespace_check(self._writer, "_dbName", "commandElement")

    def gen_op_msg_request_deserializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ deserializer method definitions from OpMsgRequest."""
        # pylint: disable=invalid-name
        # Commands that have concatentate_with_db namespaces require db name as a parameter
        if not isinstance(struct, ast.Command):
            return

        struct_type_info = struct_types.get_struct_info(struct)

        func_def = struct_type_info.get_op_msg_request_deserializer_static_method().get_definition()
        with self._block('%s {' % (func_def), '}'):
            if isinstance(struct,
                          ast.Command) and struct.namespace != common.COMMAND_NAMESPACE_IGNORED:
                self._writer.write_line('NamespaceString localNS;')
                self._writer.write_line('%s object(localNS);' % (common.title_case(struct.name)))
            else:
                self._writer.write_line('%s object;' % common.title_case(struct.name))

            self._writer.write_line(
                struct_type_info.get_op_msg_request_deserializer_method().get_call('object'))
            self._writer.write_line('return object;')

        func_def = struct_type_info.get_op_msg_request_deserializer_method().get_definition()
        with self._block('%s {' % (func_def), '}'):

            # Deserialize all the fields
            field_usage_check = self._gen_fields_deserializer_common(struct, "request.body")

            # Iterate through the document sequences if we have any
            has_doc_sequence = len(
                [field for field in struct.fields if field.supports_doc_sequence])
            if has_doc_sequence:
                with self._block('for (auto&& sequence : request.sequences) {', '}'):
                    field_usage_check.add_store("sequence.name")
                    self._writer.write_empty_line()

                    first_field = True
                    for field in struct.fields:
                        # Only parse document sequence fields here
                        if not field.supports_doc_sequence:
                            continue

                        field_predicate = 'sequence.name == %s' % (_get_field_constant_name(field))

                        with self._predicate(field_predicate, not first_field):
                            field_usage_check.add(field, "sequence.name")

                            self.gen_doc_sequence_deserializer(field)

                        if first_field:
                            first_field = False

                    # End of for fields
                    # Generate strict check for extranous fields
                    if struct.strict:
                        with self._block('else {', '}'):
                            self._writer.write_line('ctxt.throwUnknownField(sequence.name);')
                self._writer.write_empty_line()

            # Check for required fields
            field_usage_check.add_final_checks()
            self._writer.write_empty_line()

            # Generate namespace check now that "$db" has been read or defaulted
            struct_type_info.gen_namespace_check(self._writer, "_dbName", "commandElement")

    def _gen_serializer_method_custom(self, field):
        # type: (ast.Field) -> None
        """Generate the serialize method definition for a custom type."""

        # Generate custom serialization
        template_params = {
            'field_name': _get_field_constant_name(field),
            'access_member': _access_member(field),
        }

        with self._with_template(template_params):
            # Is this a scalar bson C++ type?
            bson_cpp_type = cpp_types.get_bson_cpp_type(field)

            # Object types need to go through the generic custom serialization code below
            if bson_cpp_type and bson_cpp_type.has_serializer():
                if field.array:
                    self._writer.write_template(
                        'BSONArrayBuilder arrayBuilder(builder->subarrayStart(${field_name}));')
                    with self._block('for (const auto& item : ${access_member}) {', '}'):
                        expression = bson_cpp_type.gen_serializer_expression(self._writer, 'item')
                        template_params['expression'] = expression
                        self._writer.write_template('arrayBuilder.append(${expression});')
                else:
                    expression = bson_cpp_type.gen_serializer_expression(self._writer,
                                                                         _access_member(field))
                    template_params['expression'] = expression
                    self._writer.write_template('builder->append(${field_name}, ${expression});')

            elif field.bson_serialization_type[0] == 'any':
                # Any types are special
                # Array variants - we pass an array builder
                # Non-array variants - we pass the field name they should use, and a BSONObjBuilder.
                method_name = writer.get_method_name(field.serializer)
                template_params['method_name'] = method_name

                if field.array:
                    self._writer.write_template(
                        'BSONArrayBuilder arrayBuilder(builder->subarrayStart(${field_name}));')
                    with self._block('for (const auto& item : ${access_member}) {', '}'):
                        # Call a method like class::method(BSONArrayBuilder*)
                        self._writer.write_template('item.${method_name}(&arrayBuilder);')
                else:
                    if writer.is_function(field.serializer):
                        # Call a method like method(value, StringData, BSONObjBuilder*)
                        self._writer.write_template(
                            '${method_name}(${access_member}, ${field_name}, builder);')
                    else:
                        # Call a method like class::method(StringData, BSONObjBuilder*)
                        self._writer.write_template(
                            '${access_member}.${method_name}(${field_name}, builder);')

            else:
                method_name = writer.get_method_name(field.serializer)
                template_params['method_name'] = method_name

                if field.array:
                    self._writer.write_template(
                        'BSONArrayBuilder arrayBuilder(builder->subarrayStart(${field_name}));')
                    with self._block('for (const auto& item : ${access_member}) {', '}'):
                        self._writer.write_line(
                            'BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());')
                        self._writer.write_template('item.${method_name}(&subObjBuilder);')
                else:
                    self._writer.write_template('${access_member}.${method_name}(builder);')

    def _gen_serializer_method_struct(self, field):
        # type: (ast.Field) -> None
        """Generate the serialize method definition for a struct type."""

        template_params = {
            'field_name': _get_field_constant_name(field),
            'access_member': _access_member(field),
        }

        with self._with_template(template_params):

            if field.chained:
                # Just directly call the serializer for chained structs without opening up a nested
                # document.
                self._writer.write_template('${access_member}.serialize(builder);')
            elif field.array:
                self._writer.write_template(
                    'BSONArrayBuilder arrayBuilder(builder->subarrayStart(${field_name}));')
                with self._block('for (const auto& item : ${access_member}) {', '}'):
                    self._writer.write_line(
                        'BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());')
                    self._writer.write_line('item.serialize(&subObjBuilder);')
            else:
                self._writer.write_template(
                    'BSONObjBuilder subObjBuilder(builder->subobjStart(${field_name}));')
                self._writer.write_template('${access_member}.serialize(&subObjBuilder);')

    def _gen_serializer_methods_common(self, struct, is_op_msg_request):
        # type: (ast.Struct, bool) -> None
        """Generate the serialize method definition."""

        struct_type_info = struct_types.get_struct_info(struct)

        # Check all required fields have been specified
        required_fields = [
            _get_has_field_member_name(field) for field in struct.fields
            if _is_required_serializer_field(field)
        ]

        if required_fields:
            assert_fields_set = ' && '.join(required_fields)
            self._writer.write_line('invariant(%s);' % assert_fields_set)
            self._writer.write_empty_line()

        # Serialize the namespace as the first field
        if isinstance(struct, ast.Command):
            struct_type_info = struct_types.get_struct_info(struct)
            struct_type_info.gen_serializer(self._writer)

        for field in struct.fields:
            # If fields are meant to be ignored during deserialization, there is no need to
            # serialize. Ignored fields have no backing storage.
            if field.ignore:
                continue

            # The $db injected field should only be inject when serializing to OpMsgRequest. In the
            # BSON case, it will be injected in the generic command layer.
            if field.serialize_op_msg_request_only and not is_op_msg_request:
                continue

            member_name = _get_field_member_name(field)

            # Is this a scalar bson C++ type?
            bson_cpp_type = cpp_types.get_bson_cpp_type(field)

            needs_custom_serializer = field.serializer or (bson_cpp_type and
                                                           bson_cpp_type.has_serializer())

            optional_block_start = None
            if field.optional:
                optional_block_start = 'if (%s.is_initialized()) {' % (member_name)
            elif field.struct_type or needs_custom_serializer or field.array:
                # Introduce a new scope for required nested object serialization.
                optional_block_start = '{'

            with self._block(optional_block_start, '}'):

                if not field.struct_type:
                    if needs_custom_serializer:
                        self._gen_serializer_method_custom(field)
                    else:
                        # Generate default serialization using BSONObjBuilder::append
                        # Note: BSONObjBuilder::append has overrides for std::vector also
                        self._writer.write_line(
                            'builder->append(%s, %s);' %
                            (_get_field_constant_name(field), _access_member(field)))
                else:
                    self._gen_serializer_method_struct(field)

            # Add a blank line after each block
            self._writer.write_empty_line()

        # Append passthrough elements
        if isinstance(struct, ast.Command):
            self._writer.write_line(
                "IDLParserErrorContext::appendGenericCommandArguments(commandPassthroughFields, _knownFields, builder);"
            )
            self._writer.write_empty_line()

    def gen_bson_serializer_method(self, struct):
        # type: (ast.Struct) -> None
        """Generate the serialize method definition."""

        struct_type_info = struct_types.get_struct_info(struct)

        with self._block('%s {' % (struct_type_info.get_serializer_method().get_definition()), '}'):
            self._gen_serializer_methods_common(struct, False)

    def gen_to_bson_serializer_method(self, struct):
        # type: (ast.Struct) -> None
        """Generate the toBSON method definition."""
        struct_type_info = struct_types.get_struct_info(struct)

        with self._block('%s {' % (struct_type_info.get_to_bson_method().get_definition()), '}'):
            self._writer.write_line('BSONObjBuilder builder;')
            self._writer.write_line(struct_type_info.get_serializer_method().get_call(None).replace(
                "builder", "&builder"))
            self._writer.write_line('return builder.obj();')

    def gen_op_msg_request_serializer_method(self, struct):
        # type: (ast.Struct) -> None
        """Generate the serialzer method definition for OpMsgRequest."""
        # pylint: disable=invalid-name
        if not isinstance(struct, ast.Command):
            return

        struct_type_info = struct_types.get_struct_info(struct)

        with self._block('%s {' %
                         (struct_type_info.get_op_msg_request_serializer_method().get_definition()),
                         '}'):
            self._writer.write_line('BSONObjBuilder localBuilder;')
            self._writer.write_line('BSONObjBuilder* builder = &localBuilder;')

            self._gen_serializer_methods_common(struct, True)

            self._writer.write_line('OpMsgRequest request;')
            self._writer.write_line('request.body = localBuilder.obj();')
            self._writer.write_line('return request;')

    def gen_string_constants_definitions(self, struct):
        # type: (ast.Struct) -> None
        # pylint: disable=invalid-name
        """Generate a StringData constant for field name in the cpp file."""

        # Generate a sorted list of string constants

        sorted_fields = sorted([field for field in struct.fields], key=lambda f: f.cpp_name)

        for field in sorted_fields:
            self._writer.write_line(
                common.template_args(
                    'constexpr StringData ${class_name}::${constant_name};',
                    class_name=common.title_case(struct.name),
                    constant_name=_get_field_constant_name(field)))

        if isinstance(struct, ast.Command):
            self._writer.write_line(
                common.template_args(
                    'constexpr StringData ${class_name}::kCommandName;',
                    class_name=common.title_case(struct.name)))

    def gen_enum_definition(self, idl_enum):
        # type: (ast.Enum) -> None
        """Generate the definitions for an enum's supporting functions."""
        enum_type_info = enum_types.get_type_info(idl_enum)

        enum_type_info.gen_deserializer_definition(self._writer)
        self._writer.write_empty_line()

        enum_type_info.gen_serializer_definition(self._writer)
        self._writer.write_empty_line()

    def gen_known_fields_declaration(self, struct):
        # type: (ast.Struct) -> None
        """Generate the known fields declaration."""
        if not isinstance(struct, ast.Command):
            return

        block_name = common.template_args(
            'const std::vector<StringData> ${class_name}::_knownFields {',
            class_name=common.title_case(struct.name))
        with self._block(block_name, "};"):
            sorted_fields = sorted([field for field in struct.fields], key=lambda f: f.cpp_name)

            for field in sorted_fields:
                self._writer.write_line(
                    common.template_args(
                        '${class_name}::${constant_name},',
                        class_name=common.title_case(struct.name),
                        constant_name=_get_field_constant_name(field)))

            self._writer.write_line(
                common.template_args(
                    '${class_name}::kCommandName,', class_name=common.title_case(struct.name)))

    def generate(self, spec, header_file_name):
        # type: (ast.IDLAST, unicode) -> None
        """Generate the C++ header to a stream."""
        self.gen_file_header()

        # Generate include for generated header first
        self.gen_include(header_file_name)
        self.write_empty_line()

        # Generate system includes second
        header_list = [
            'bitset',
            'set',
        ]

        for include in header_list:
            self.gen_system_include(include)

        self.write_empty_line()

        # Generate mongo includes third
        header_list = [
            'mongo/bson/bsonobjbuilder.h',
            'mongo/db/commands.h',
        ]
        header_list.sort()

        for include in header_list:
            self.gen_include(include)

        self.write_empty_line()

        # Generate namesapce
        with self.gen_namespace_block(spec.globals.cpp_namespace):
            self.write_empty_line()

            for idl_enum in spec.enums:
                self.gen_description_comment(idl_enum.description)
                self.gen_enum_definition(idl_enum)

            for struct in spec.structs:
                self.gen_string_constants_definitions(struct)
                self.write_empty_line()

                # Write known fields declaration for command
                self.gen_known_fields_declaration(struct)
                self.write_empty_line()

                # Write constructor
                self.gen_constructors(struct)
                self.write_empty_line()

                # Write deserializers
                self.gen_bson_deserializer_methods(struct)
                self.write_empty_line()

                self.gen_op_msg_request_deserializer_methods(struct)
                self.write_empty_line()

                # Write serializer
                self.gen_bson_serializer_method(struct)
                self.write_empty_line()

                # Write OpMsgRequest serializer
                self.gen_op_msg_request_serializer_method(struct)
                self.write_empty_line()

                # Write toBSON
                self.gen_to_bson_serializer_method(struct)
                self.write_empty_line()


def _generate_header(spec, file_name):
    # type: (ast.IDLAST, unicode) -> None
    """Generate a C++ header."""
    stream = io.StringIO()
    text_writer = writer.IndentedTextWriter(stream)

    header = _CppHeaderFileWriter(text_writer)

    header.generate(spec)

    # Generate structs
    with io.open(file_name, mode='wb') as file_handle:
        file_handle.write(stream.getvalue().encode())


def _generate_source(spec, file_name, header_file_name):
    # type: (ast.IDLAST, unicode, unicode) -> None
    """Generate a C++ source file."""
    stream = io.StringIO()
    text_writer = writer.IndentedTextWriter(stream)

    source = _CppSourceFileWriter(text_writer)

    source.generate(spec, header_file_name)

    # Generate structs
    with io.open(file_name, mode='wb') as file_handle:
        file_handle.write(stream.getvalue().encode())


def generate_code(spec, output_base_dir, header_file_name, source_file_name):
    # type: (ast.IDLAST, unicode, unicode, unicode) -> None
    """Generate a C++ header and source file from an idl.ast tree."""

    _generate_header(spec, header_file_name)

    if output_base_dir:
        include_h_file_name = os.path.relpath(
            os.path.normpath(header_file_name), os.path.normpath(output_base_dir))
    else:
        include_h_file_name = os.path.abspath(os.path.normpath(header_file_name))

    # Normalize to POSIX style for consistency across Windows and POSIX.
    include_h_file_name = include_h_file_name.replace("\\", "/")

    _generate_source(spec, source_file_name, include_h_file_name)
