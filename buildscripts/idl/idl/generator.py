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

import io
import os
import string
import sys
import textwrap
from typing import List, Mapping, Union

from . import ast
from . import bson
from . import common
from . import cpp_types
from . import writer


def _get_field_member_name(field):
    # type: (ast.Field) -> unicode
    """Get the C++ class member name for a field."""
    return '_%s' % (common.camel_case(field.cpp_name))


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


class _NamespaceScopeBlock(object):
    """Generate an unindented blocks for a list of namespaces, and do not indent the contents."""

    def __init__(self, indented_writer, namespaces):
        # type: (writer.IndentedTextWriter, List[unicode]) -> None
        """Create a block."""
        self._writer = indented_writer
        self._namespaces = namespaces

    def __enter__(self):
        # type: () -> None
        """Write the beginning of the block and do not indent."""
        for namespace in self._namespaces:
            self._writer.write_unindented_line('namespace %s {' % (namespace))

    def __exit__(self, *args):
        # type: (*str) -> None
        """Write the end of the block and do not change indentation."""
        self._namespaces.reverse()

        for namespace in self._namespaces:
            self._writer.write_unindented_line('}  // namespace %s' % (namespace))


class _FieldUsageChecker(object):
    """Check for duplicate fields, and required fields as needed."""

    def __init__(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Create a field usage checker."""
        self._writer = indented_writer  # type: writer.IndentedTextWriter
        self.fields = []  # type: List[ast.Field]

        # TODO: use a more optimal data type
        self._writer.write_line('std::set<StringData> usedFields;')

    def add_store(self):
        # type: () -> None
        """Create the C++ field store initialization code."""
        self._writer.write_line('auto push_result = usedFields.insert(fieldName);')
        with writer.IndentedScopedBlock(self._writer, 'if (push_result.second == false) {', '}'):
            self._writer.write_line('ctxt.throwDuplicateField(element);')

    def add(self, field):
        # type: (ast.Field) -> None
        """Add a field to track."""
        self.fields.append(field)

    def add_final_checks(self):
        # type: () -> None
        """Output the code to check for missing fields."""
        for field in self.fields:
            if (not field.optional) and (not field.ignore) and (not field.chained):
                with writer.IndentedScopedBlock(self._writer,
                                                'if (usedFields.find("%s") == usedFields.end()) {' %
                                                (field.name), '}'):
                    if field.default:
                        self._writer.write_line('%s = %s;' %
                                                (_get_field_member_name(field), field.default))
                    else:
                        self._writer.write_line('ctxt.throwMissingField("%s");' % (field.name))


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
        # type: (unicode) -> _NamespaceScopeBlock
        """Generate a namespace block."""
        namespace_list = namespace.split("::")

        return _NamespaceScopeBlock(self._writer, namespace_list)

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

    def gen_serializer_methods(self, class_name):
        # type: (unicode) -> None
        """Generate a serializer method declarations."""
        self._writer.write_line(
            'static %s parse(const IDLParserErrorContext& ctxt, const BSONObj& object);' %
            (common.title_case(class_name)))
        self._writer.write_line('void serialize(BSONObjBuilder* builder) const;')
        self._writer.write_empty_line()

    def gen_protected_serializer_methods(self):
        # type: () -> None
        # pylint: disable=invalid-name
        """Generate protected serializer method declarations."""

        self._writer.write_line(
            'void parseProtected(const IDLParserErrorContext& ctxt, const BSONObj& object);')
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
                    'const ${param_type} get${method_name}() const { ${body} }')

    def gen_setter(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ setter definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        param_type = cpp_type_info.get_getter_setter_type()
        member_name = _get_field_member_name(field)

        template_params = {
            'method_name': common.title_case(field.cpp_name),
            'member_name': member_name,
            'param_type': param_type,
            'body': cpp_type_info.get_setter_body(member_name)
        }

        with self._with_template(template_params):
            self._writer.write_template('void set${method_name}(${param_type} value) & { ${body} }')

        self._writer.write_empty_line()

    def gen_member(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ class member definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        member_type = cpp_type_info.get_storage_type()
        member_name = _get_field_member_name(field)

        self._writer.write_line('%s %s;' % (member_type, member_name))

    def gen_string_constants_declarations(self, struct):
        # type: (ast.Struct) -> None
        # pylint: disable=invalid-name
        """Generate a StringData constant for field name."""
        field_names = sorted([field.name for field in struct.fields if not field.ignore])

        for field_name in field_names:
            self._writer.write_line(
                common.template_args(
                    'static constexpr auto k${constant_name}FieldName = "${field_name}"_sd;',
                    constant_name=common.title_case(field_name),
                    field_name=field_name))

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

                    # Generate a sorted list of string constants
                    self.gen_string_constants_declarations(struct)
                    self.write_empty_line()

                    # Write constructor
                    self.gen_serializer_methods(struct.name)

                    # Write getters & setters
                    for field in struct.fields:
                        if not field.ignore:
                            if field.description:
                                self.gen_description_comment(field.description)
                            self.gen_getter(field)
                            self.gen_setter(field)

                    self.write_unindented_line('protected:')
                    self.gen_protected_serializer_methods()

                    self.write_unindented_line('private:')

                    # Write member variables
                    for field in struct.fields:
                        if not field.ignore:
                            self.gen_member(field)

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
            self._writer.write_line('IDLParserErrorContext tempContext("%s", &ctxt);' %
                                    (field.name))
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
                    return common.template_args(
                        "$method_name(${expression})",
                        method_name=method_name,
                        expression=expression)
                else:
                    # BSONObjects are allowed to be pass through without deserialization
                    assert len(
                        field.bson_serialization_type) == 1 and field.bson_serialization_type[
                            0] == 'object'
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
        self._writer.write_line('const IDLParserErrorContext arrayCtxt("%s", &ctxt);' %
                                (field.name))
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

    def gen_field_deserializer(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ deserializer piece for a field."""
        if field.array:
            self._gen_array_deserializer(field)
            return

        if field.chained:
            # Do not generate a predicate check since we always call these deserializers.

            if field.struct_type:
                # Do not generate a new parser context, reuse the current one since we are not
                # entering a nested document.
                expression = '%s::parse(ctxt, bsonObject)' % (common.title_case(field.struct_type))
            else:
                method_name = writer.get_method_name_from_qualified_method_name(field.deserializer)
                expression = "%s(bsonObject)" % (method_name)

            self._writer.write_line('%s = %s;' % (_get_field_member_name(field), expression))
        else:
            # May be an empty block if the type is 'any'
            with self._predicate(_get_bson_type_check('element', 'ctxt', field)):
                object_value = self._gen_field_deserializer_expression('element', field)
                self._writer.write_line('%s = %s;' % (_get_field_member_name(field), object_value))

    def gen_deserializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ deserializer method definitions."""
        func_def = common.template_args(
            '${class_name} ${class_name}::parse (const IDLParserErrorContext& ctxt,' +
            'const BSONObj& bsonObject)',
            class_name=common.title_case(struct.name))
        with self._block('%s {' % (func_def), '}'):
            self._writer.write_line('%s object;' % common.title_case(struct.name))
            self._writer.write_line('object.parseProtected(ctxt, bsonObject);')
            self._writer.write_line('return object;')

        func_def = 'void %s::parseProtected(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject)' % (
            common.title_case(struct.name))
        with self._block('%s {' % (func_def), '}'):

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
                    # Do not parse chained fields as fields since they are actually chained types.
                    if field.chained:
                        continue

                    field_predicate = 'fieldName == "%s"' % (field.name)
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

            # Parse chained types
            for field in struct.fields:
                if not field.chained:
                    continue

                # Simply generate deserializers since these are all 'any' types
                self.gen_field_deserializer(field)
            self._writer.write_empty_line()

            # Check for required fields
            field_usage_check.add_final_checks()
            self._writer.write_empty_line()

    def _gen_serializer_method_custom(self, field):
        # type: (ast.Field) -> None
        """Generate the serialize method definition for a custom type."""

        # Generate custom serialization
        template_params = {
            'field_name': field.name,
            'access_member': _access_member(field),
        }

        with self._with_template(template_params):
            # Is this a scalar bson C++ type?
            bson_cpp_type = cpp_types.get_bson_cpp_type(field)

            # Object types need to go through the generic custom serialization code below
            if bson_cpp_type and bson_cpp_type.has_serializer():
                if field.array:
                    self._writer.write_template(
                        'BSONArrayBuilder arrayBuilder(builder->subarrayStart("${field_name}"));')
                    with self._block('for (const auto& item : ${access_member}) {', '}'):
                        expression = bson_cpp_type.gen_serializer_expression(self._writer, 'item')
                        template_params['expression'] = expression
                        self._writer.write_template('arrayBuilder.append(${expression});')
                else:
                    expression = bson_cpp_type.gen_serializer_expression(self._writer,
                                                                         _access_member(field))
                    template_params['expression'] = expression
                    self._writer.write_template('builder->append("${field_name}", ${expression});')

            elif field.bson_serialization_type[0] == 'any':
                # Any types are special
                # Array variants - we pass an array builder
                # Non-array variants - we pass the field name they should use, and a BSONObjBuilder.
                method_name = writer.get_method_name(field.serializer)
                template_params['method_name'] = method_name

                if field.array:
                    self._writer.write_template(
                        'BSONArrayBuilder arrayBuilder(builder->subarrayStart("${field_name}"));')
                    with self._block('for (const auto& item : ${access_member}) {', '}'):
                        # Call a method like class::method(BSONArrayBuilder*)
                        self._writer.write_template('item.${method_name}(&arrayBuilder);')
                else:
                    # Call a method like class::method(StringData, BSONObjBuilder*)
                    self._writer.write_template(
                        '${access_member}.${method_name}("${field_name}", builder);')
            else:
                method_name = writer.get_method_name(field.serializer)
                template_params['method_name'] = method_name

                if field.array:
                    self._writer.write_template(
                        'BSONArrayBuilder arrayBuilder(builder->subarrayStart("${field_name}"));')
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
            'field_name': field.name,
            'access_member': _access_member(field),
        }

        with self._with_template(template_params):

            if field.chained:
                # Just directly call the serializer for chained structs without opening up a nested
                # document.
                self._writer.write_template('${access_member}.serialize(builder);')
            elif field.array:
                self._writer.write_template(
                    'BSONArrayBuilder arrayBuilder(builder->subarrayStart("${field_name}"));')
                with self._block('for (const auto& item : ${access_member}) {', '}'):
                    self._writer.write_line(
                        'BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());')
                    self._writer.write_line('item.serialize(&subObjBuilder);')
            else:
                self._writer.write_template(
                    'BSONObjBuilder subObjBuilder(builder->subobjStart("${field_name}"));')
                self._writer.write_template('${access_member}.serialize(&subObjBuilder);')

    def gen_serializer_method(self, struct):
        # type: (ast.Struct) -> None
        """Generate the serialize method definition."""

        with self._block('void %s::serialize(BSONObjBuilder* builder) const {' %
                         common.title_case(struct.name), '}'):

            for field in struct.fields:
                # If fields are meant to be ignored during deserialization, there is not need to serialize them
                if field.ignore:
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
                            self._writer.write_line('builder->append("%s", %s);' %
                                                    (field.name, _access_member(field)))
                    else:
                        self._gen_serializer_method_struct(field)

                # Add a blank line after each block
                self._writer.write_empty_line()

    def gen_string_constants_definitions(self, struct):
        # type: (ast.Struct) -> None
        # pylint: disable=invalid-name
        """Generate a StringData constant for field name in the cpp file."""

        # Generate a sorted list of string constants

        field_names = sorted([field.name for field in struct.fields if not field.ignore])

        for field_name in field_names:
            self._writer.write_line(
                common.template_args(
                    'constexpr StringData ${class_name}::k${constant_name}FieldName;',
                    class_name=common.title_case(struct.name),
                    constant_name=common.title_case(field_name)))

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
                self.gen_string_constants_definitions(struct)
                self.write_empty_line()

                # Write deserializer
                self.gen_deserializer_methods(struct)
                self.write_empty_line()

                # Write serializer
                self.gen_serializer_method(struct)
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
