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
"""IDL C++ Code Generator."""

from __future__ import absolute_import, print_function, unicode_literals

import io
import os
import string
import sys
import textwrap
# from typing import List, Union

from . import ast
from . import bson

# Number of spaces to indent code
_INDENT_SPACE_COUNT = 4


def _title_case(name):
    # type: (unicode) -> unicode
    """Return a CapitalCased version of a string."""
    return name[0:1].upper() + name[1:]


def _camel_case(name):
    # type: (unicode) -> unicode
    """Return a camelCased version of a string."""
    return name[0:1].lower() + name[1:]


def _get_method_name(name):
    # type: (unicode) -> unicode
    """Get a method name from a fully qualified method name."""
    pos = name.rfind('::')
    if pos == -1:
        return name
    return name[pos + 2:]


def _get_method_name_from_qualified_method_name(name):
    # type: (unicode) -> unicode
    # pylint: disable=invalid-name
    """Get a method name from a fully qualified method name."""
    # TODO: in the future, we may want to support full-qualified calls to static methods
    prefix = 'mongo::'
    pos = name.find(prefix)
    if pos == -1:
        return name

    return name[len(prefix):]


def _is_primitive_type(cpp_type):
    # type: (unicode) -> bool
    """Return True if a cpp_type is a primitive type and should not be returned as reference."""
    return cpp_type in [
        'bool', 'double', 'std::int32_t', 'std::uint32_t', 'std::uint64_t', 'std::int64_t'
    ]


def _is_view_type(cpp_type):
    # type: (unicode) -> bool
    """Return True if a cpp_type should be returned as a view type from an IDL class."""
    if cpp_type == 'std::string':
        return True

    return False


def _get_view_type(cpp_type):
    # type: (unicode) -> unicode
    """Map a C++ type to its C++ view type if needed."""
    if cpp_type == 'std::string':
        cpp_type = 'StringData'

    return cpp_type


def _get_view_type_to_base_method(cpp_type):
    # type: (unicode) -> unicode
    """Map a C++ View type to its C++ base type."""
    assert _is_view_type(cpp_type)

    return "toString"


def _get_field_cpp_type(field):
    # type: (ast.Field) -> unicode
    """Get the C++ type name for a field."""
    assert field.cpp_type is not None or field.struct_type is not None

    if field.struct_type:
        cpp_type = _title_case(field.struct_type)
    else:
        cpp_type = field.cpp_type

    return cpp_type


def _qualify_optional_type(cpp_type, field):
    # type: (unicode, ast.Field) -> unicode
    """Qualify the type if the field is optional."""
    if field.optional:
        return 'boost::optional<%s>' % (cpp_type)

    return cpp_type


def _get_field_parameter_type(field):
    # type: (ast.Field) -> unicode
    """Get the C++ type name for a parameter for a field."""
    assert field.cpp_type is not None or field.struct_type is not None

    cpp_type = _get_view_type(_get_field_cpp_type(field))

    return _qualify_optional_type(cpp_type, field)


def _get_field_member_type(field):
    # type: (ast.Field) -> unicode
    """Get the C++ type name for a class member for a field."""
    cpp_type = _get_field_cpp_type(field)

    return _qualify_optional_type(cpp_type, field)


def _get_field_member_name(field):
    # type: (ast.Field) -> unicode
    """Get the C++ class member name for a field."""
    return '_%s' % (_camel_case(field.name))


def _get_bson_type_check(field):
    # type: (ast.Field) -> unicode
    """Get the C++ bson type check for a field."""
    bson_types = field.bson_serialization_type
    if len(bson_types) == 1:
        if bson_types[0] == 'any':
            # Skip BSON valiation when any
            return None

        if not bson_types[0] == 'bindata':
            return 'ctxt.checkAndAssertType(element, %s)' % bson.cpp_bson_type_name(bson_types[0])
        else:
            return 'ctxt.checkAndAssertBinDataType(element, %s)' % bson.cpp_bindata_subtype_type_name(
                field.bindata_subtype)
    else:
        type_list = '{%s}' % (', '.join([bson.cpp_bson_type_name(b) for b in bson_types]))
        return 'ctxt.checkAndAssertTypes(element, %s)' % type_list


def _access_member(field):
    # type: (ast.Field) -> unicode
    """Get the declaration to access a member for a field."""
    member_name = _get_field_member_name(field)

    if not field.optional:
        return '%s' % (member_name)

    # optional types need a method call to access their values
    return '%s.get()' % (member_name)


def fill_spaces(count):
    # type: (int) -> unicode
    """Fill a string full of spaces."""
    fill = ''
    for _ in range(count * _INDENT_SPACE_COUNT):
        fill += ' '

    return fill


def indent_text(count, unindented_text):
    # type: (int, unicode) -> unicode
    """Indent each line of a multi-line string."""
    lines = unindented_text.splitlines()
    fill = fill_spaces(count)
    return '\n'.join(fill + line for line in lines)


class _IndentedTextWriter(object):
    """
    A simple class to manage writing indented lines of text.

    Supports both writing indented lines, and unindented lines.
    Use write_empty_line() instead of write_line('') to avoid lines
    full of blank spaces.
    """

    def __init__(self, stream):
        # type: (io.StringIO) -> None
        """Create an indented text writer."""
        self._stream = stream
        self._indent = 0

    def write_unindented_line(self, msg):
        # type: (unicode) -> None
        """Write an unindented line to the stream."""
        self._stream.write(msg)
        self._stream.write("\n")

    def indent(self):
        # type: () -> None
        """Indent the text by one level."""
        self._indent += 1

    def unindent(self):
        # type: () -> None
        """Unindent the text by one level."""
        assert self._indent > 0
        self._indent -= 1

    def write_line(self, msg):
        # type: (unicode) -> None
        """Write a line to the stream."""
        self._stream.write(indent_text(self._indent, msg))
        self._stream.write("\n")

    def write_empty_line(self):
        # type: () -> None
        """Write a line to the stream."""
        self._stream.write("\n")


class _EmptyBlock(object):
    """Do not generate an indented block."""

    def __init__(self):
        # type: () -> None
        """Create an empty block."""
        pass

    def __enter__(self):
        # type: () -> None
        """Do nothing."""
        pass

    def __exit__(self, *args):
        # type: (*str) -> None
        """Do nothing."""
        pass


class _UnindentedScopedBlock(object):
    """Generate an unindented block, and do not indent the contents."""

    def __init__(self, writer, opening, closing):
        # type: (_IndentedTextWriter, unicode, unicode) -> None
        """Create a block."""
        self._writer = writer
        self._opening = opening
        self._closing = closing

    def __enter__(self):
        # type: () -> None
        """Write the beginning of the block and do not indent."""
        self._writer.write_unindented_line(self._opening)

    def __exit__(self, *args):
        # type: (*str) -> None
        """Write the end of the block and do not change indentation."""
        self._writer.write_unindented_line(self._closing)


class _IndentedScopedBlock(object):
    """Generate a block, and indent the contents."""

    def __init__(self, writer, opening, closing):
        # type: (_IndentedTextWriter, unicode, unicode) -> None
        """Create a block."""
        self._writer = writer
        self._opening = opening
        self._closing = closing

    def __enter__(self):
        # type: () -> None
        """Write the beginning of the block and then indent."""
        self._writer.write_line(self._opening)
        self._writer.indent()

    def __exit__(self, *args):
        # type: (*str) -> None
        """Unindent the block and print the ending."""
        self._writer.unindent()
        self._writer.write_line(self._closing)


class _FieldUsageChecker(object):
    """Check for duplicate fields, and required fields as needed."""

    def __init__(self, writer):
        # type: (_IndentedTextWriter) -> None
        """Create a field usage checker."""
        self._writer = writer  # type: _IndentedTextWriter
        self.fields = []  # type: List[ast.Field]

        # TODO: use a more optimal data type
        self._writer.write_line('std::set<StringData> usedFields;')

    def add_store(self):
        # type: () -> None
        """Create the C++ field store initialization code."""
        self._writer.write_line('auto push_result = usedFields.insert(fieldName);')
        with _IndentedScopedBlock(self._writer, 'if (push_result.second == false) {', '}'):
            self._writer.write_line('ctxt.throwDuplicateField(element);')

    def add(self, field):
        # type: (ast.Field) -> None
        """Add a field to track."""
        self.fields.append(field)

    def add_final_checks(self):
        # type: () -> None
        """Output the code to check for missing fields."""
        for field in self.fields:
            if (not field.optional) and (not field.ignore):
                with _IndentedScopedBlock(self._writer,
                                          'if (usedFields.find("%s") == usedFields.end()) {' %
                                          (field.name), '}'):
                    if field.default:
                        self._writer.write_line('object.%s = %s;' %
                                                (_get_field_member_name(field), field.default))
                    else:
                        self._writer.write_line('ctxt.throwMissingField("%s");' % (field.name))


class _CppFileWriterBase(object):
    """
    C++ File writer.

    Encapsulates low level knowledge of how to print a C++ file.
    Relies on caller to orchestrate calls correctly though.
    """

    def __init__(self, writer):
        # type: (_IndentedTextWriter) -> None
        """Create a C++ code writer."""
        self._writer = writer  # type: _IndentedTextWriter

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
        # type: (unicode) -> _UnindentedScopedBlock
        """Generate a namespace block."""
        # TODO: support namespace strings which consist of '::' delimited namespaces
        return _UnindentedScopedBlock(self._writer, 'namespace %s {' % (namespace),
                                      '}  // namespace %s' % (namespace))

    def gen_description_comment(self, description):
        # type: (unicode) -> None
        """Generate a multiline comment with the description from the IDL."""
        self._writer.write_line(
            textwrap.dedent("""\
        /**
         * %s
         */""" % (description)))

    def _block(self, opening, closing):
        # type: (unicode, unicode) -> Union[_IndentedScopedBlock,_EmptyBlock]
        """Generate an indented block if opening is not empty."""
        if not opening:
            return _EmptyBlock()

        return _IndentedScopedBlock(self._writer, opening, closing)

    def _predicate(self, check_str, use_else_if=False):
        # type: (unicode, bool) -> Union[_IndentedScopedBlock,_EmptyBlock]
        """
        Generate an if block if the condition is not-empty.

        Generate 'else if' instead of use_else_if is True.
        """
        if not check_str:
            return _EmptyBlock()

        conditional = 'if'
        if use_else_if:
            conditional = 'else if'

        return _IndentedScopedBlock(self._writer, '%s (%s) {' % (conditional, check_str), '}')


class _CppHeaderFileWriter(_CppFileWriterBase):
    """C++ .h File writer."""

    def __init__(self, writer):
        # type: (_IndentedTextWriter) -> None
        """Create a C++ .cpp file code writer."""
        super(_CppHeaderFileWriter, self).__init__(writer)

    def gen_class_declaration_block(self, class_name):
        # type: (unicode) -> _IndentedScopedBlock
        """Generate a class declaration block."""
        return _IndentedScopedBlock(self._writer, 'class %s {' % _title_case(class_name), '};')

    def gen_serializer_methods(self, class_name):
        # type: (unicode) -> None
        """Generate a serializer method declarations."""
        self._writer.write_line(
            'static %s parse(const IDLParserErrorContext& ctxt, const BSONObj& object);' %
            (_title_case(class_name)))
        self._writer.write_line('void serialize(BSONObjBuilder* builder) const;')
        self._writer.write_empty_line()

    def gen_getter(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ getter definition for a field."""
        cpp_type = _get_field_cpp_type(field)
        param_type = _get_field_parameter_type(field)
        member_name = _get_field_member_name(field)

        optional_ampersand = ""
        disable_xvalue = False
        if not _is_view_type(cpp_type):
            if not field.optional and not _is_primitive_type(cpp_type):
                optional_ampersand = '&'
                disable_xvalue = True
            body = 'return %s;' % (member_name)
        else:
            body = 'return %s{%s};' % (param_type, member_name)
            disable_xvalue = True

        # Generate a getter that disables xvalue for view types (i.e. StringData), constructed
        # optional types, and non-primitive types.
        if disable_xvalue:
            self._writer.write_line('const %s%s get%s() const& { %s }' %
                                    (param_type, optional_ampersand, _title_case(field.name), body))
            self._writer.write_line("const %s%s get%s() && = delete;" %
                                    (param_type, optional_ampersand, _title_case(field.name)))
        else:
            self._writer.write_line('const %s%s get%s() const { %s }' %
                                    (param_type, optional_ampersand, _title_case(field.name), body))

    def gen_setter(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ setter definition for a field."""
        cpp_type = _get_field_cpp_type(field)
        param_type = _get_field_parameter_type(field)
        member_name = _get_field_member_name(field)

        if _is_view_type(cpp_type):
            if not field.optional:
                self._writer.write_line('void set%s(%s value) & { %s = value.%s(); }' %
                                        (_title_case(field.name), param_type, member_name,
                                         _get_view_type_to_base_method(cpp_type)))

            else:
                # We need to convert between two different types of optional<T> and yet retain the
                # ability for the user to specific an uninitialized optional. This occurs for
                # mongo::StringData and std::string paired together.
                with self._block('void set%s(%s value) {' % (_title_case(field.name), param_type),
                                 "}"):
                    self._writer.write_line(
                        textwrap.dedent("""\
                    if (value.is_initialized()) {
                        %s = value.get().%s();
                    } else {
                        %s = boost::none;
                    }
                    """ % (member_name, _get_view_type_to_base_method(cpp_type), member_name)))

        else:
            self._writer.write_line('void set%s(%s value) { %s = std::move(value); }' %
                                    (_title_case(field.name), param_type, member_name))
        self._writer.write_empty_line()

    def gen_member(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ class member definition for a field."""
        member_type = _get_field_member_type(field)
        member_name = _get_field_member_name(field)

        self._writer.write_line('%s %s;' % (member_type, member_name))

    def generate(self, spec):
        # type: (ast.IDLAST) -> None
        """Generate the C++ header to a stream."""
        self.gen_file_header()

        self._writer.write_unindented_line('#pragma once')
        self.write_empty_line()

        # Generate system includes first
        header_list = [
            'algorithm',
            'boost/optional.hpp',
            'cstdint',
            'string',
        ]

        header_list.sort()

        for include in header_list:
            self.gen_system_include(include)

        self.write_empty_line()

        # Generate user includes second
        header_list = [
            'mongo/base/string_data.h',
            'mongo/bson/bsonobj.h',
            'mongo/idl/idl_parser.h',
        ] + spec.globals.cpp_includes

        header_list.sort()

        for include in header_list:
            self.gen_include(include)

        self.write_empty_line()

        # Generate namesapce
        with self.gen_namespace_block(spec.globals.cpp_namespace):
            self.write_empty_line()

            for struct in spec.structs:
                self.gen_description_comment(struct.description)
                with self.gen_class_declaration_block(struct.name):
                    self.write_unindented_line('public:')

                    # Write constructor
                    self.gen_serializer_methods(struct.name)

                    # Write getters & setters
                    for field in struct.fields:
                        if not field.ignore:
                            if field.description:
                                self.gen_description_comment(field.description)
                            self.gen_getter(field)
                            self.gen_setter(field)

                    self.write_unindented_line('private:')

                    # Write member variables
                    for field in struct.fields:
                        if not field.ignore:
                            self.gen_member(field)

                self.write_empty_line()


class _CppSourceFileWriter(_CppFileWriterBase):
    """C++ .cpp File writer."""

    def __init__(self, writer):
        # type: (_IndentedTextWriter) -> None
        """Create a C++ .cpp file code writer."""
        super(_CppSourceFileWriter, self).__init__(writer)

    def gen_field_deserializer(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ deserializer piece for a few field."""
        # May be an empty block if the type is any
        type_predicate = _get_bson_type_check(field)

        with self._predicate(type_predicate):

            if field.struct_type:
                self._writer.write_line('IDLParserErrorContext tempContext("%s", &ctxt);' %
                                        (field.name))
                self._writer.write_line('const auto localObject = element.Obj();')
                self._writer.write_line('object.%s = %s::parse(tempContext, localObject);' % (
                    _get_field_member_name(field), _title_case(field.struct_type)))
            elif 'BSONElement::' in field.deserializer:
                method_name = _get_method_name(field.deserializer)
                self._writer.write_line('object.%s = element.%s();' %
                                        (_get_field_member_name(field), method_name))
            else:
                # Custom method, call the method on object
                # TODO: avoid this string hack in the future
                if len(field.bson_serialization_type) == 1 and field.bson_serialization_type[
                        0] == 'string':
                    # Call a method like: Class::method(StringData value)
                    self._writer.write_line('auto tempValue = element.valueStringData();')

                    method_name = _get_method_name(field.deserializer)

                    self._writer.write_line('object.%s = %s(tempValue);' %
                                            (_get_field_member_name(field), method_name))
                elif len(field.bson_serialization_type) == 1 and field.bson_serialization_type[
                        0] == 'object':
                    # Call a method like: Class::method(const BSONObj& value)
                    method_name = _get_method_name_from_qualified_method_name(field.deserializer)
                    self._writer.write_line('const BSONObj localObject = element.Obj();')
                    self._writer.write_line('object.%s = %s(localObject);' %
                                            (_get_field_member_name(field), method_name))
                else:
                    # Call a method like: Class::method(const BSONElement& value)
                    method_name = _get_method_name_from_qualified_method_name(field.deserializer)

                    self._writer.write_line('object.%s = %s(element);' %
                                            (_get_field_member_name(field), method_name))

    def gen_deserializer_method(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ deserializer method definition."""

        with self._block(
                '%s %s::parse(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject) {' %
            (_title_case(struct.name), _title_case(struct.name)), '}'):

            self._writer.write_line('%s object;' % _title_case(struct.name))

            field_usage_check = _FieldUsageChecker(self._writer)
            self._writer.write_empty_line()

            with self._block('for (const auto& element : bsonObject) {', '}'):

                self._writer.write_line('const auto fieldName = element.fieldNameStringData();')
                self._writer.write_empty_line()

                # TODO: generate command namespace string check
                field_usage_check.add_store()
                self._writer.write_empty_line()

                first_field = True
                for field in struct.fields:
                    field_predicate = 'fieldName == "%s"' % field.name
                    field_usage_check.add(field)

                    with self._predicate(field_predicate, not first_field):
                        if field.ignore:
                            self._writer.write_line('// ignore field')
                        else:
                            self.gen_field_deserializer(field)

                    if first_field:
                        first_field = False

                # End of for fields
                # Generate strict check for extranous fields
                if struct.strict:
                    with self._block('else {', '}'):
                        self._writer.write_line('ctxt.throwUnknownField(fieldName);')

            self._writer.write_empty_line()

            # Check for required fields
            field_usage_check.add_final_checks()
            self._writer.write_empty_line()

            self._writer.write_line('return object;')

    def gen_serializer_method(self, struct):
        # type: (ast.Struct) -> None
        """Generate the serialize method definition."""

        with self._block('void %s::serialize(BSONObjBuilder* builder) const {' %
                         _title_case(struct.name), '}'):

            for field in struct.fields:
                # If fields are meant to be ignored during deserialization, there is not need to serialize them
                if field.ignore:
                    continue

                member_name = _get_field_member_name(field)

                optional_block_start = None
                if field.optional:
                    optional_block_start = 'if (%s) {' % (member_name)
                elif field.struct_type:
                    # Introduce a new scope for required nested object serialization.
                    optional_block_start = '{'

                with self._block(optional_block_start, '}'):

                    if not field.struct_type:
                        if field.serializer:
                            # Generate custom serialization
                            method_name = _get_method_name(field.serializer)

                            if len(field.bson_serialization_type) == 1 and \
                                field.bson_serialization_type[0] == 'string':
                                # TODO: expand this out to be less then a string only hack
                                self._writer.write_line('auto tempValue = %s.%s();' %
                                                        (_access_member(field), method_name))
                                self._writer.write_line(
                                    'builder->append("%s", std::move(tempValue));' % (field.name))
                            else:
                                self._writer.write_line('%s.%s(builder);' %
                                                        (_access_member(field), method_name))

                        else:
                            # Generate default serialization using BSONObjBuilder::append
                            self._writer.write_line('builder->append("%s", %s);' %
                                                    (field.name, _access_member(field)))

                    else:
                        self._writer.write_line(
                            'BSONObjBuilder subObjBuilder(builder->subobjStart("%s"));' %
                            (field.name))
                        self._writer.write_line('%s.serialize(&subObjBuilder);' %
                                                (_access_member(field)))
                # Add a blank line after each block
                self._writer.write_empty_line()

    def generate(self, spec, header_file_name):
        # type: (ast.IDLAST, unicode) -> None
        """Generate the C++ header to a stream."""
        self.gen_file_header()

        # Generate include for generated header first
        self.gen_include(header_file_name)
        self.write_empty_line()

        # Generate system includes second
        self.gen_system_include('set')
        self.write_empty_line()

        # Generate mongo includes third
        self.gen_include('mongo/bson/bsonobjbuilder.h')
        self.write_empty_line()

        # Generate namesapce
        with self.gen_namespace_block(spec.globals.cpp_namespace):
            self.write_empty_line()

            for struct in spec.structs:
                # Write deserializer
                self.gen_deserializer_method(struct)
                self.write_empty_line()

                # Write serializer
                self.gen_serializer_method(struct)
                self.write_empty_line()


def _generate_header(spec, file_name):
    # type: (ast.IDLAST, unicode) -> None
    """Generate a C++ header."""
    stream = io.StringIO()
    text_writer = _IndentedTextWriter(stream)

    header = _CppHeaderFileWriter(text_writer)

    header.generate(spec)

    # Generate structs
    with io.open(file_name, mode='wb') as file_handle:
        file_handle.write(stream.getvalue().encode())


def _generate_source(spec, file_name, header_file_name):
    # type: (ast.IDLAST, unicode, unicode) -> None
    """Generate a C++ source file."""
    stream = io.StringIO()
    text_writer = _IndentedTextWriter(stream)

    source = _CppSourceFileWriter(text_writer)

    source.generate(spec, header_file_name)

    # Generate structs
    with io.open(file_name, mode='wb') as file_handle:
        file_handle.write(stream.getvalue().encode())


def generate_code(spec, output_base_dir, header_file_name, source_file_name):
    # type: (ast.IDLAST, unicode, unicode, unicode) -> None
    """Generate a C++ header and source file from an idl.ast tree."""

    _generate_header(spec, header_file_name)

    include_h_file_name = os.path.relpath(
        os.path.normpath(header_file_name), os.path.normpath(output_base_dir))

    # Normalize to POSIX style for consistency across Windows and POSIX.
    include_h_file_name = include_h_file_name.replace("\\", "/")

    _generate_source(spec, source_file_name, include_h_file_name)
