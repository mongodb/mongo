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
# pylint: disable=too-many-lines
"""IDL C++ Code Generator."""

from __future__ import absolute_import, print_function, unicode_literals

from abc import ABCMeta, abstractmethod
import io
import os
import string
import sys
import textwrap
import uuid
from typing import cast, Dict, List, Mapping, Tuple, Union

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


def _get_field_member_setter_name(field):
    # type: (ast.Field) -> unicode
    """Get the C++ class setter name for a field."""
    return "set%s" % (common.title_case(field.cpp_name))


def _get_field_member_getter_name(field):
    # type: (ast.Field) -> unicode
    """Get the C++ class getter name for a field."""
    return "get%s" % (common.title_case(field.cpp_name))


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
    return not field.ignore and not field.optional and not field.default and not field.chained and not field.chained_struct_field


def _get_field_constant_name(field):
    # type: (ast.Field) -> unicode
    """Get the C++ string constant name for a field."""
    return common.template_args('k${constant_name}FieldName', constant_name=common.title_case(
        field.cpp_name))


def _get_field_member_validator_name(field):
    # type (ast.Field) -> unicode
    """Get the name of the validator method for this field."""
    return 'validate%s' % common.title_case(field.cpp_name)


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
        return '%s.checkAndAssertBinDataType(%s, %s)' % (
            ctxt_name, bson_element, bson.cpp_bindata_subtype_type_name(field.bindata_subtype))
    else:
        type_list = '{%s}' % (', '.join([bson.cpp_bson_type_name(b) for b in bson_types]))
        return '%s.checkAndAssertTypes(%s, %s)' % (ctxt_name, bson_element, type_list)


def _get_all_fields(struct):
    # type: (ast.Struct) -> List[ast.Field]
    """Get a list of all the fields, including the command field."""
    all_fields = []
    if isinstance(struct, ast.Command) and struct.command_field:
        all_fields.append(struct.command_field)

    all_fields += struct.fields

    return sorted([field for field in all_fields], key=lambda f: f.cpp_name)


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
        with writer.IndentedScopedBlock(self._writer,
                                        'if (MONGO_unlikely(push_result.second == false)) {', '}'):
            self._writer.write_line('ctxt.throwDuplicateField(%s);' % (field_name))

    def add(self, field, bson_element_variable):
        # type: (ast.Field, unicode) -> None
        if not field in self._fields:
            self._fields.append(field)

    def add_final_checks(self):
        # type: () -> None
        for field in self._fields:
            if (not field.optional) and (not field.ignore) and (not field.chained):
                pred = 'if (MONGO_unlikely(usedFields.find(%s) == usedFields.end())) {' % \
                    (_get_field_constant_name(field))
                with writer.IndentedScopedBlock(self._writer, pred, '}'):
                    if field.default:
                        if field.enum_type:
                            self._writer.write_line('%s = %s::%s;' %
                                                    (_get_field_member_name(field), field.cpp_type,
                                                     field.default))
                        else:
                            self._writer.write_line('%s = %s;' % (_get_field_member_name(field),
                                                                  field.default))
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

            self._writer.write_line('const size_t %s = %d;' % (_gen_field_usage_constant(field),
                                                               bit_id))
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

        with writer.IndentedScopedBlock(self._writer, 'if (MONGO_unlikely(usedFields[%s])) {' %
                                        (_gen_field_usage_constant(field)), '}'):
            self._writer.write_line('ctxt.throwDuplicateField(%s);' % (bson_element_variable))
        self._writer.write_empty_line()

        self._writer.write_line('usedFields.set(%s);' % (_gen_field_usage_constant(field)))
        self._writer.write_empty_line()

    def add_final_checks(self):
        # type: () -> None
        """Output the code to check for missing fields."""
        with writer.IndentedScopedBlock(self._writer, 'if (MONGO_unlikely(!usedFields.all())) {',
                                        '}'):
            for field in self._fields:
                if (not field.optional) and (not field.ignore):
                    with writer.IndentedScopedBlock(self._writer, 'if (!usedFields[%s]) {' %
                                                    (_gen_field_usage_constant(field)), '}'):
                        if field.default:
                            if field.chained_struct_field:
                                self._writer.write_line(
                                    '%s.%s(%s);' %
                                    (_get_field_member_name(field.chained_struct_field),
                                     _get_field_member_setter_name(field), field.default))
                            elif field.enum_type:
                                self._writer.write_line('%s = %s::%s;' %
                                                        (_get_field_member_name(field),
                                                         field.cpp_type, field.default))
                            else:
                                self._writer.write_line('%s = %s;' % (_get_field_member_name(field),
                                                                      field.default))
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


# Turn a python string into a C++ literal.
def _encaps(val):
    # type: (unicode) -> unicode
    if val is None:
        return '""'

    for i in ["\\", '"', "'"]:
        if i in val:
            val = val.replace(i, '\\' + i)
    return '"' + val + '"'


# Turn a list of pything strings into a C++ initializer list.
def _encaps_list(vals):
    # type: (List[unicode]) -> unicode
    if vals is None:
        return '{}'

    return '{' + ', '.join([_encaps(v) for v in vals]) + '}'


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

    def get_initializer_lambda(self, decl, unused=False, return_type=None):
        # type: (unicode, bool, unicode) -> writer.IndentedScopedBlock
        """Generate an indented block lambda initializing an outer scope variable."""
        prefix = 'MONGO_COMPILER_VARIABLE_UNUSED ' if unused else ''
        prefix = prefix + decl + ' = ([]'
        if return_type:
            prefix = prefix + '() -> ' + return_type
        return writer.IndentedScopedBlock(self._writer, prefix + ' {', '})();')

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

    def _predicate(self, check_str, use_else_if=False, constexpr=False):
        # type: (unicode, bool, bool) -> Union[writer.IndentedScopedBlock,writer.EmptyBlock]
        """
        Generate an if block if the condition is not-empty.

        Generate 'else if' instead of use_else_if is True.
        """
        if not check_str:
            return writer.EmptyBlock()

        conditional = 'if'
        if use_else_if:
            conditional = 'else if'

        if constexpr:
            conditional = conditional + ' constexpr'

        return writer.IndentedScopedBlock(self._writer, '%s (%s) {' % (conditional, check_str), '}')

    def _condition(self, condition, preprocessor_only=False):
        # type: (ast.Condition, bool) -> writer.WriterBlock
        """Generate one or more blocks for multiple conditional types."""

        if not condition:
            return writer.EmptyBlock()

        blocks = []  # type: List[writer.WriterBlock]
        if condition.preprocessor:
            blocks.append(
                writer.UnindentedBlock(self._writer, '#if ' + condition.preprocessor, '#endif'))

        if not preprocessor_only:
            if condition.constexpr:
                blocks.append(self._predicate(condition.constexpr, constexpr=True))
            if condition.expr:
                blocks.append(self._predicate(condition.expr))

        if not blocks:
            return writer.EmptyBlock()

        if len(blocks) == 1:
            return blocks[0]

        return writer.MultiBlock(blocks)


class _CppHeaderFileWriter(_CppFileWriterBase):
    """C++ .h File writer."""

    def gen_class_declaration_block(self, class_name):
        # type: (unicode) -> writer.IndentedScopedBlock
        """Generate a class declaration block."""
        return writer.IndentedScopedBlock(self._writer,
                                          'class %s {' % common.title_case(class_name), '};')

    def gen_class_constructors(self, struct):
        # type: (ast.Struct) -> None
        """Generate the declarations for the class constructors."""
        struct_type_info = struct_types.get_struct_info(struct)

        constructor = struct_type_info.get_constructor_method()
        self._writer.write_line(constructor.get_declaration())

        required_constructor = struct_type_info.get_required_constructor_method()
        if len(required_constructor.args) != len(constructor.args):
            self._writer.write_line(required_constructor.get_declaration())

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

    def gen_getter(self, struct, field):
        # type: (ast.Struct, ast.Field) -> None
        """Generate the C++ getter definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        param_type = cpp_type_info.get_getter_setter_type()
        member_name = _get_field_member_name(field)

        if cpp_type_info.return_by_reference():
            param_type += "&"

        template_params = {
            'method_name': _get_field_member_getter_name(field),
            'param_type': param_type,
            'body': cpp_type_info.get_getter_body(member_name),
            'const_type': 'const ' if cpp_type_info.is_const_type() else '',
        }

        # Generate a getter that disables xvalue for view types (i.e. StringData), constructed
        # optional types, and non-primitive types.
        with self._with_template(template_params):

            if field.chained_struct_field:
                self._writer.write_template(
                    '${const_type} ${param_type} ${method_name}() const { return %s.%s(); }' %
                    ((_get_field_member_name(field.chained_struct_field),
                      _get_field_member_getter_name(field))))

            elif cpp_type_info.disable_xvalue():
                self._writer.write_template(
                    'const ${param_type} ${method_name}() const& { ${body} }')
                self._writer.write_template('void ${method_name}() && = delete;')

            elif field.struct_type:
                # Support mutable accessors
                self._writer.write_template(
                    'const ${param_type} ${method_name}() const { ${body} }')

                if not struct.immutable:
                    self._writer.write_template('${param_type} ${method_name}() { ${body} }')
            else:
                self._writer.write_template(
                    '${const_type}${param_type} ${method_name}() const { ${body} }')

    def gen_validators(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ validators definition for a field."""
        assert field.validator

        param_type = field.cpp_type
        if not cpp_types.is_primitive_type(param_type):
            param_type += '&'

        template_params = {
            'method_name': _get_field_member_validator_name(field),
            'param_type': param_type,
        }

        with self._with_template(template_params):
            # Declare method implemented in C++ file.
            self._writer.write_template('void ${method_name}(const ${param_type} value);')
            self._writer.write_template(
                'void ${method_name}(IDLParserErrorContext& ctxt, const ${param_type} value);')

        self._writer.write_empty_line()

    def gen_setter(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ setter definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        param_type = cpp_type_info.get_getter_setter_type()
        member_name = _get_field_member_name(field)

        post_body = ''
        if _is_required_serializer_field(field):
            post_body = '%s = true;' % (_get_has_field_member_name(field))

        validator_method_name = ''
        if field.validator is not None:
            validator_method_name = _get_field_member_validator_name(field)

        template_params = {
            'method_name': _get_field_member_setter_name(field),
            'member_name': member_name,
            'param_type': param_type,
            'body': cpp_type_info.get_setter_body(member_name, validator_method_name),
            'post_body': post_body,
        }

        with self._with_template(template_params):
            self._writer.write_template(
                'void ${method_name}(${param_type} value) & { ${body} ${post_body} }')

        self._writer.write_empty_line()

    def gen_member(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ class member definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        member_type = cpp_type_info.get_storage_type()
        member_name = _get_field_member_name(field)

        if field.default and not field.constructed:
            if field.enum_type:
                self._writer.write_line('%s %s{%s::%s};' % (member_type, member_name,
                                                            field.cpp_type, field.default))
            else:
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

        for field in _get_all_fields(struct):
            self._writer.write_line(
                common.template_args('static constexpr auto ${constant_name} = "${field_name}"_sd;',
                                     constant_name=_get_field_constant_name(field),
                                     field_name=field.name))

        if isinstance(struct, ast.Command):
            self._writer.write_line(
                common.template_args('static constexpr auto kCommandName = "${struct_name}"_sd;',
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
                    common.template_args('${name} ${value},', name=enum_value.name,
                                         value=enum_type_info.get_cpp_value_assignment(enum_value)))

    def gen_op_msg_request_methods(self, command):
        # type: (ast.Command) -> None
        """Generate the methods for a command."""
        if command.command_field:
            self.gen_getter(command, command.command_field)
        else:
            struct_type_info = struct_types.get_struct_info(command)
            struct_type_info.gen_getter_method(self._writer)

        self._writer.write_empty_line()

    def gen_op_msg_request_member(self, command):
        # type: (ast.Command) -> None
        """Generate the class members for a command."""
        if command.command_field:
            self.gen_member(command.command_field)
        else:
            struct_type_info = struct_types.get_struct_info(command)
            struct_type_info.gen_member(self._writer)

        self._writer.write_empty_line()

    def gen_known_fields_declaration(self):
        # type: () -> None
        """Generate a known fields vector for a command."""
        self._writer.write_line("static const std::vector<StringData> _knownFields;")
        self.write_empty_line()

    def gen_comparison_operators_declarations(self, struct):
        # type: (ast.Struct) -> None
        """Generate comparison operators declarations for the type."""
        # pylint: disable=invalid-name

        sorted_fields = sorted([
            field for field in struct.fields if (not field.ignore) and field.comparison_order != -1
        ], key=lambda f: f.comparison_order)
        fields = [_get_field_member_name(field) for field in sorted_fields]

        with self._block("auto relationalTie() const {", "}"):
            self._writer.write_line('return std::tie(%s);' % (', '.join(fields)))

        for rel_op in ['==', '!=', '<', '>', '<=', '>=']:
            self.write_empty_line()
            decl = common.template_args(
                "friend bool operator${rel_op}(const ${class_name}& left, const ${class_name}& right) {",
                rel_op=rel_op, class_name=common.title_case(struct.name))

            with self._block(decl, "}"):
                self._writer.write_line('return left.relationalTie() %s right.relationalTie();' %
                                        (rel_op))

        self.write_empty_line()

    def gen_extern_declaration(self, vartype, varname, condition):
        # type: (unicode, unicode, ast.Condition) -> None
        """Generate externs for storage declaration."""
        if (vartype is None) or (varname is None):
            return

        with self._condition(condition, preprocessor_only=True):
            idents = varname.split('::')
            decl = idents.pop()
            for ns in idents:
                self._writer.write_line('namespace %s {' % (ns))

            self._writer.write_line('extern %s %s;' % (vartype, decl))

            for ns in reversed(idents):
                self._writer.write_line('}  // namespace ' + ns)

        if idents:
            self.write_empty_line()

    def generate(self, spec):
        # type: (ast.IDLAST) -> None
        """Generate the C++ header to a stream."""
        # pylint: disable=too-many-branches,too-many-statements
        self.gen_file_header()

        self._writer.write_unindented_line('#pragma once')
        self.write_empty_line()

        # Generate system includes first
        header_list = [
            'algorithm',
            'boost/optional.hpp',
            'cstdint',
            'string',
            'tuple',
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
            'mongo/rpc/op_msg.h',
        ] + spec.globals.cpp_includes

        if spec.configs:
            header_list.append('mongo/util/options_parser/option_description.h')

        if spec.server_parameters:
            header_list.append('mongo/util/synchronized_value.h')

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
                with self.gen_class_declaration_block(struct.cpp_name):
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
                            self.gen_getter(struct, field)
                            if not struct.immutable and not field.chained_struct_field:
                                self.gen_setter(field)

                    if struct.generate_comparison_operators:
                        self.gen_comparison_operators_declarations(struct)

                    self.write_unindented_line('protected:')
                    self.gen_protected_serializer_methods(struct)

                    # Write private validators
                    if [field for field in struct.fields if field.validator]:
                        self.write_unindented_line('private:')
                        for field in struct.fields:
                            if not field.ignore and not struct.immutable and \
                                not field.chained_struct_field and field.validator:
                                self.gen_validators(field)

                    self.write_unindented_line('private:')

                    # Write command member variables
                    if isinstance(struct, ast.Command):
                        self.gen_known_fields_declaration()
                        self.write_empty_line()

                        self.gen_op_msg_request_member(struct)

                    # Write member variables
                    for field in struct.fields:
                        if not field.ignore and not field.chained_struct_field:
                            self.gen_member(field)

                    # Write serializer member variables
                    # Note: we write these out second to ensure the bit fields can be packed by
                    # the compiler.
                    for field in struct.fields:
                        if _is_required_serializer_field(field):
                            self.gen_serializer_member(field)

                self.write_empty_line()

            for scp in spec.server_parameters:
                self.gen_extern_declaration(scp.cpp_vartype, scp.cpp_varname, scp.condition)
            for opt in spec.configs:
                self.gen_extern_declaration(opt.cpp_vartype, opt.cpp_varname, opt.condition)


class _CppSourceFileWriter(_CppFileWriterBase):
    """C++ .cpp File writer."""

    def __init__(self, indented_writer, target_arch):
        # type: (writer.IndentedTextWriter, unicode) -> None
        """Create a C++ .cpp file code writer."""
        self._target_arch = target_arch
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

        # Custom method, call the method on object.
        bson_cpp_type = cpp_types.get_bson_cpp_type(field)

        if bson_cpp_type:
            # Call a static class method with the signature:
            # Class Class::method(StringData value)
            # or
            # Class::method(const BSONObj& value)
            expression = bson_cpp_type.gen_deserializer_expression(self._writer, element_name)
            if field.deserializer:
                method_name = writer.get_method_name_from_qualified_method_name(field.deserializer)

                # For fields which are enums, pass a IDLParserErrorContext
                if field.enum_type:
                    self._writer.write_line('IDLParserErrorContext tempContext(%s, &ctxt);' %
                                            (_get_field_constant_name(field)))
                    return common.template_args("${method_name}(tempContext, ${expression})",
                                                method_name=method_name, expression=expression)
                return common.template_args("${method_name}(${expression})",
                                            method_name=method_name, expression=expression)

            # BSONObjects are allowed to be pass through without deserialization
            assert field.bson_serialization_type == ['object']
            return expression

        # Call a static class method with the signature:
        # Class Class::method(const BSONElement& value)
        method_name = writer.get_method_name_from_qualified_method_name(field.deserializer)

        return '%s(%s)' % (method_name, element_name)

    def _gen_array_deserializer(self, field, bson_element):
        # type: (ast.Field, unicode) -> None
        """Generate the C++ deserializer piece for an array field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        cpp_type = cpp_type_info.get_type_name()

        self._writer.write_line('std::uint32_t expectedFieldNumber{0};')
        self._writer.write_line('const IDLParserErrorContext arrayCtxt(%s, &ctxt);' %
                                (_get_field_constant_name(field)))
        self._writer.write_line('std::vector<%s> values;' % (cpp_type))
        self._writer.write_empty_line()

        self._writer.write_line('const BSONObj arrayObject = %s.Obj();' % (bson_element))

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

        if field.chained_struct_field:
            self._writer.write_line('%s.%s(std::move(values));' %
                                    (_get_field_member_name(field.chained_struct_field),
                                     _get_field_member_setter_name(field)))
        else:
            self._writer.write_line('%s = std::move(values);' % (_get_field_member_name(field)))

    def _gen_usage_check(self, field, bson_element, field_usage_check):
        # type: (ast.Field, unicode, _FieldUsageCheckerBase) -> None
        """Generate the field usage check and insert the required field check."""
        if field_usage_check:
            field_usage_check.add(field, bson_element)

            if _is_required_serializer_field(field):
                self._writer.write_line('%s = true;' % (_get_has_field_member_name(field)))

    def gen_field_deserializer(self, field, bson_object, bson_element, field_usage_check):
        # type: (ast.Field, unicode, unicode, _FieldUsageCheckerBase) -> None
        """Generate the C++ deserializer piece for a field."""
        if field.array:
            self._gen_usage_check(field, bson_element, field_usage_check)

            self._gen_array_deserializer(field, bson_element)
            return

        def validate_and_assign_or_uassert(field, expression):
            # type: (ast.Field, unicode) -> None
            """Perform field value validation post-assignment."""
            field_name = _get_field_member_name(field)
            if field.validator is None:
                self._writer.write_line('%s = %s;' % (field_name, expression))
                return

            with self._block('{', '}'):
                self._writer.write_line('auto value = %s;' % (expression))
                self._writer.write_line('%s(value);' % (_get_field_member_validator_name(field)))
                self._writer.write_line('%s = std::move(value);' % (field_name))

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

            self._gen_usage_check(field, bson_element, field_usage_check)
            validate_and_assign_or_uassert(field, expression)

        else:
            predicate = _get_bson_type_check(bson_element, 'ctxt', field)
            if predicate:
                predicate = "MONGO_likely(%s)" % (predicate)
            with self._predicate(predicate):

                self._gen_usage_check(field, bson_element, field_usage_check)

                object_value = self._gen_field_deserializer_expression(bson_element, field)
                if field.chained_struct_field:
                    # No need for explicit validation as setter will throw for us.
                    self._writer.write_line('%s.%s(%s);' %
                                            (_get_field_member_name(field.chained_struct_field),
                                             _get_field_member_setter_name(field), object_value))
                else:
                    validate_and_assign_or_uassert(field, object_value)

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
                if field.deserializer:
                    array_value = '%s(sequenceObject)' % (field.deserializer)
                else:
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

    def _gen_constructor(self, struct, constructor, default_init):
        # type: (ast.Struct, struct_types.MethodInfo, bool) -> None
        """Generate the C++ constructor definition."""

        initializers = ['_%s(std::move(%s))' % (arg.name, arg.name) for arg in constructor.args]

        # Serialize non-has fields first
        # Initialize int and other primitive fields to -1 to prevent Coverity warnings.
        if default_init:
            for field in struct.fields:
                needs_init = field.cpp_type and not field.array and cpp_types.is_primitive_scalar_type(
                    field.cpp_type)
                if _is_required_serializer_field(field) and needs_init:
                    initializers.append(
                        '%s(%s)' %
                        (_get_field_member_name(field),
                         cpp_types.get_primitive_scalar_type_default_value(field.cpp_type)))

        # Serialize the _dbName field second
        initializes_db_name = False
        if [arg for arg in constructor.args if arg.name == 'nss']:
            if [field for field in struct.fields if field.serialize_op_msg_request_only]:
                initializers.append('_dbName(nss.db().toString())')
                initializes_db_name = True

        # Serialize has fields third
        # Add _has{FIELD} bool members to ensure fields are set before serialization.
        for field in struct.fields:
            if _is_required_serializer_field(field) and not (field.name == "$db"
                                                             and initializes_db_name):
                if default_init:
                    initializers.append('%s(false)' % _get_has_field_member_name(field))
                else:
                    initializers.append('%s(true)' % _get_has_field_member_name(field))

        if initializes_db_name:
            initializers.append('_hasDbName(true)')

        initializers_str = ''
        if initializers:
            initializers_str = ': ' + ', '.join(initializers)

        with self._block('%s %s {' % (constructor.get_definition(), initializers_str), '}'):
            self._writer.write_line('// Used for initialization only')

    def gen_constructors(self, struct):
        # type: (ast.Struct) -> None
        """Generate all the C++ constructor definitions."""

        struct_type_info = struct_types.get_struct_info(struct)
        constructor = struct_type_info.get_constructor_method()

        self._gen_constructor(struct, constructor, True)

        required_constructor = struct_type_info.get_required_constructor_method()
        if len(required_constructor.args) != len(constructor.args):
            #print(struct.name + ": "+  str(required_constructor.args))
            self._gen_constructor(struct, required_constructor, False)

    def _gen_command_deserializer(self, struct, bson_object):
        # type: (ast.Struct, unicode) -> None
        """Generate the command field deserializer."""

        if isinstance(struct, ast.Command) and struct.command_field:
            with self._block('{', '}'):
                self.gen_field_deserializer(struct.command_field, bson_object, "commandElement",
                                            None)
        else:
            struct_type_info = struct_types.get_struct_info(struct)

            # Generate namespace check now that "$db" has been read or defaulted
            struct_type_info.gen_namespace_check(self._writer, "_dbName", "commandElement")

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
                if field.chained and not field.chained_struct_field:
                    continue

                field_predicate = 'fieldName == %s' % (_get_field_constant_name(field))

                with self._predicate(field_predicate, not first_field):

                    if field.ignore:
                        field_usage_check.add(field, "element")

                        self._writer.write_line('// ignore field')
                    else:
                        self.gen_field_deserializer(field, bson_object, "element",
                                                    field_usage_check)

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
                        command_predicate = "!mongo::isGenericArgument(fieldName)"

                    with self._predicate(command_predicate):
                        self._writer.write_line('ctxt.throwUnknownField(fieldName);')

        # Parse chained structs if not inlined
        # Parse chained types always here
        for field in struct.fields:
            if not field.chained or \
                    (field.chained and field.struct_type and struct.inline_chained_structs):
                continue

            # Simply generate deserializers since these are all 'any' types
            self.gen_field_deserializer(field, bson_object, "element", None)
            self._writer.write_empty_line()

        self._writer.write_empty_line()

        return field_usage_check

    def get_bson_deserializer_static_common(self, struct, static_method_info, method_info):
        # type: (ast.Struct, struct_types.MethodInfo, struct_types.MethodInfo) -> None
        """Generate the C++ deserializer static method."""
        # pylint: disable=invalid-name
        func_def = static_method_info.get_definition()

        with self._block('%s {' % (func_def), '}'):
            if isinstance(struct,
                          ast.Command) and struct.namespace != common.COMMAND_NAMESPACE_IGNORED:
                if struct.namespace == common.COMMAND_NAMESPACE_TYPE:
                    cpp_type_info = cpp_types.get_cpp_type(struct.command_field)

                    if struct.command_field.cpp_type and cpp_types.is_primitive_scalar_type(
                            struct.command_field.cpp_type):
                        self._writer.write_line('%s localCmdType(%s);' %
                                                (cpp_type_info.get_storage_type(),
                                                 cpp_types.get_primitive_scalar_type_default_value(
                                                     struct.command_field.cpp_type)))
                    else:
                        self._writer.write_line('%s localCmdType;' %
                                                (cpp_type_info.get_storage_type()))
                    self._writer.write_line('%s object(localCmdType);' %
                                            (common.title_case(struct.cpp_name)))
                elif struct.namespace == common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB:
                    self._writer.write_line('NamespaceString localNS;')
                    self._writer.write_line('%s object(localNS);' %
                                            (common.title_case(struct.cpp_name)))
            else:
                self._writer.write_line('%s object;' % common.title_case(struct.cpp_name))

            self._writer.write_line(method_info.get_call('object'))
            self._writer.write_line('return object;')

    def _compare_and_return_status(self, op, limit, field, optional_param):
        # type: (unicode, Union[int, float], ast.Field, unicode) -> None
        """Throw an error on comparison failure."""
        with self._block('if (!(value %s %s)) {' % (op, repr(limit)), '}'):
            self._writer.write_line('throwComparisonError<%s>(%s"%s", "%s"_sd, value, %s);' %
                                    (field.cpp_type, optional_param, field.name, op, limit))

    def _gen_field_validator(self, struct, field, optional_params):
        # type: (ast.Struct, ast.Field, Tuple[unicode, unicode]) -> None
        """Generate non-trivial field validators."""
        validator = field.validator

        param_type = field.cpp_type
        if not cpp_types.is_primitive_type(param_type):
            param_type += '&'

        method_template = {
            'class_name': common.title_case(struct.name),
            'method_name': _get_field_member_validator_name(field),
            'param_type': param_type,
            'optional_param': optional_params[0],
        }

        with self._with_template(method_template):
            self._writer.write_template(
                'void ${class_name}::${method_name}(${optional_param}const ${param_type} value)')
            with self._block('{', '}'):
                if validator.gt is not None:
                    self._compare_and_return_status('>', validator.gt, field, optional_params[1])
                if validator.gte is not None:
                    self._compare_and_return_status('>=', validator.gte, field, optional_params[1])
                if validator.lt is not None:
                    self._compare_and_return_status('<', validator.lt, field, optional_params[1])
                if validator.lte is not None:
                    self._compare_and_return_status('<=', validator.lte, field, optional_params[1])

                if validator.callback is not None:
                    self._writer.write_line('uassertStatusOK(%s(value));' % (validator.callback))

        self._writer.write_empty_line()

    def gen_field_validators(self, struct):
        # type: (ast.Struct) -> None
        """Generate non-trivial field validators."""
        for field in struct.fields:
            if field.validator is None:
                # Fields without validators are implemented in the header.
                continue

            for optional_params in [('IDLParserErrorContext& ctxt, ', 'ctxt, '), ('', '')]:
                self._gen_field_validator(struct, field, optional_params)

    def gen_bson_deserializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ deserializer method definitions."""
        struct_type_info = struct_types.get_struct_info(struct)

        self.get_bson_deserializer_static_common(struct,
                                                 struct_type_info.get_deserializer_static_method(),
                                                 struct_type_info.get_deserializer_method())

        func_def = struct_type_info.get_deserializer_method().get_definition()
        with self._block('%s {' % (func_def), '}'):

            # Deserialize all the fields
            field_usage_check = self._gen_fields_deserializer_common(struct, "bsonObject")

            # Check for required fields
            field_usage_check.add_final_checks()
            self._writer.write_empty_line()

            self._gen_command_deserializer(struct, "bsonObject")

    def gen_op_msg_request_deserializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ deserializer method definitions from OpMsgRequest."""
        # pylint: disable=invalid-name
        # Commands that have concatentate_with_db namespaces require db name as a parameter
        if not isinstance(struct, ast.Command):
            return

        struct_type_info = struct_types.get_struct_info(struct)

        self.get_bson_deserializer_static_common(
            struct, struct_type_info.get_op_msg_request_deserializer_static_method(),
            struct_type_info.get_op_msg_request_deserializer_method())

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

                            if _is_required_serializer_field(field):
                                self._writer.write_line('%s = true;' %
                                                        (_get_has_field_member_name(field)))

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

            self._gen_command_deserializer(struct, "request.body")

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
                    expression = bson_cpp_type.gen_serializer_expression(
                        self._writer, _access_member(field))
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

    def _gen_serializer_method_common(self, field):
        # type: (ast.Field) -> None
        """Generate the serialize method definition."""
        member_name = _get_field_member_name(field)

        # Is this a scalar bson C++ type?
        bson_cpp_type = cpp_types.get_bson_cpp_type(field)

        needs_custom_serializer = field.serializer or (bson_cpp_type
                                                       and bson_cpp_type.has_serializer())

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
                        'builder->append(%s, %s);' % (_get_field_constant_name(field),
                                                      _access_member(field)))
            else:
                self._gen_serializer_method_struct(field)

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
            if struct.command_field:
                self._gen_serializer_method_common(struct.command_field)
            else:
                struct_type_info = struct_types.get_struct_info(struct)
                struct_type_info.gen_serializer(self._writer)

        for field in struct.fields:
            # If fields are meant to be ignored during deserialization, there is no need to
            # serialize. Ignored fields have no backing storage.
            if field.ignore:
                continue

            if field.chained_struct_field:
                continue

            # The $db injected field should only be inject when serializing to OpMsgRequest. In the
            # BSON case, it will be injected in the generic command layer.
            if field.serialize_op_msg_request_only and not is_op_msg_request:
                continue

            # Serialize fields that can be document sequence as document sequences so as not to
            # generate the BSON body >= 16 MB.
            if field.supports_doc_sequence and is_op_msg_request:
                continue

            self._gen_serializer_method_common(field)

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

    def _gen_doc_sequence_serializer(self, struct):
        # type: (ast.Struct) -> None
        """Generate the serialize method portion for fields which can be document sequence."""

        for field in struct.fields:
            if not field.supports_doc_sequence:
                continue

            member_name = _get_field_member_name(field)

            optional_block_start = '{'
            if field.optional:
                optional_block_start = 'if (%s.is_initialized()) {' % (member_name)

            with self._block(optional_block_start, '}'):
                self._writer.write_line('OpMsg::DocumentSequence documentSequence;')
                self._writer.write_template('documentSequence.name = %s.toString();' %
                                            (_get_field_constant_name(field)))

                with self._block('for (const auto& item : %s) {' % (_access_member(field)), '}'):

                    if not field.struct_type:
                        if field.serializer:
                            self._writer.write_line('documentSequence.objs.push_back(item.%s());' %
                                                    (writer.get_method_name(field.serializer)))
                        else:
                            self._writer.write_line('documentSequence.objs.push_back(item);')
                    else:
                        self._writer.write_line('BSONObjBuilder builder;')
                        self._writer.write_line('item.serialize(&builder);')
                        self._writer.write_line('documentSequence.objs.push_back(builder.obj());')

                self._writer.write_template('request.sequences.emplace_back(documentSequence);')

            # Add a blank line after each block
            self._writer.write_empty_line()

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

            with self._block('{', '}'):
                self._writer.write_line('BSONObjBuilder* builder = &localBuilder;')

                self._gen_serializer_methods_common(struct, True)

            self._writer.write_line('OpMsgRequest request;')
            self._writer.write_line('request.body = localBuilder.obj();')

            self._gen_doc_sequence_serializer(struct)

            self._writer.write_line('return request;')

    def gen_string_constants_definitions(self, struct):
        # type: (ast.Struct) -> None
        # pylint: disable=invalid-name
        """Generate a StringData constant for field name in the cpp file."""

        for field in _get_all_fields(struct):
            self._writer.write_line(
                common.template_args('constexpr StringData ${class_name}::${constant_name};',
                                     class_name=common.title_case(struct.cpp_name),
                                     constant_name=_get_field_constant_name(field)))

        if isinstance(struct, ast.Command):
            self._writer.write_line(
                common.template_args('constexpr StringData ${class_name}::kCommandName;',
                                     class_name=common.title_case(struct.cpp_name)))

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
            class_name=common.title_case(struct.cpp_name))
        with self._block(block_name, "};"):
            sorted_fields = sorted([field for field in struct.fields], key=lambda f: f.cpp_name)

            for field in sorted_fields:
                self._writer.write_line(
                    common.template_args(
                        '${class_name}::${constant_name},', class_name=common.title_case(
                            struct.cpp_name), constant_name=_get_field_constant_name(field)))

            self._writer.write_line(
                common.template_args('${class_name}::kCommandName,', class_name=common.title_case(
                    struct.cpp_name)))

    def gen_server_parameter(self, param):
        # type: (ast.ServerParameter) -> None
        """Generate a single IDLServerParameter(WithStorage)."""
        # pylint: disable=too-many-branches
        with self._condition(param.condition):
            if param.cpp_varname is not None:
                self._writer.write_line(
                    common.template_args(
                        'auto* ret = makeIDLServerParameterWithStorage(${name}, ${storage}, ${spt});',
                        storage=param.cpp_varname, spt=param.set_at, name=_encaps(param.name)))

                if param.on_update is not None:
                    self._writer.write_line('ret->setOnUpdate(%s);' % (param.on_update))
                if param.validator is not None:
                    if param.validator.callback is not None:
                        self._writer.write_line('ret->addValidator(%s);' %
                                                (param.validator.callback))

                    for pred in ['lt', 'gt', 'lte', 'gte']:
                        bound = getattr(param.validator, pred)
                        if bound is not None:
                            self._writer.write_line(
                                'ret->addBound<idl_server_parameter_detail::%s>(%s);' %
                                (pred.upper(), bound))

                if param.redact:
                    self._writer.write_line('ret->setRedact();')

            else:
                self._writer.write_line(
                    common.template_args('auto* ret = new IDLServerParameter(${name}, ${spt});',
                                         spt=param.set_at, name=_encaps(param.name)))
                if param.from_bson:
                    self._writer.write_line('ret->setFromBSON(%s);' % (param.from_bson))

                if param.append_bson:
                    self._writer.write_line('ret->setAppendBSON(%s);' % (param.append_bson))
                elif param.redact:
                    self._writer.write_line(
                        'ret->setAppendBSON(IDLServerParameter::redactedAppendBSON);')

                self._writer.write_line('ret->setFromString(%s);' % (param.from_string))

            if param.default is not None:
                self._writer.write_line('uassertStatusOK(ret->setFromString(%s));' %
                                        (_encaps(param.default)))

            self._writer.write_line('return ret;')

        if param.condition:
            # Fallback in case any of the provided conditions are false.
            self._writer.write_line('return nullptr;')

    def gen_server_parameter_deprecated_aliases(self, param_no, param):
        # type: (int, ast.ServerParameter) -> None
        """Generate IDLServerParamterDeprecatedAlias instance."""

        for alias_no, alias in enumerate(param.deprecated_name):
            with self.get_initializer_lambda('auto* scp_%d_%d' % (param_no, alias_no), unused=True,
                                             return_type='ServerParameter*'):
                with self._condition(param.condition):
                    with self._predicate('scp_%d != nullptr' % (param_no)):
                        self._writer.write_line(
                            'return new IDLServerParameterDeprecatedAlias(%s, scp_%d);' %
                            (_encaps(alias), param_no))

                # Fallthrough in case any predicate above fails.
                self._writer.write_line('return nullptr;')

    def gen_server_parameters(self, params):
        # type: (List[ast.ServerParameter]) -> None
        """Generate IDLServerParameter instances."""

        for param in params:
            # Optional storage declarations.
            if (param.cpp_vartype is not None) and (param.cpp_varname is not None):
                with self._condition(param.condition):
                    self._writer.write_line('%s %s;' % (param.cpp_vartype, param.cpp_varname))

        with self.gen_namespace_block(''):
            # ServerParameter instances.
            for param_no, param in enumerate(params):
                self.gen_description_comment(param.description)

                with self.get_initializer_lambda('auto* scp_%d' % (param_no), unused=(len(
                        param.deprecated_name) == 0), return_type='ServerParameter*'):
                    self.gen_server_parameter(param)

                self.gen_server_parameter_deprecated_aliases(param_no, param)
                self.write_empty_line()

    def gen_config_option(self, opt, section):
        # type: (ast.ConfigOption, unicode) -> None
        """Generate Config Option instance."""

        # Derive cpp_vartype from arg_vartype if needed.
        vartype = ("moe::OptionTypeMap<moe::%s>::type" %
                   (opt.arg_vartype)) if opt.cpp_vartype is None else opt.cpp_vartype

        with self._condition(opt.condition):
            with self._block(section, ';'):
                self._writer.write_line(
                    common.template_args(
                        '.addOptionChaining(${name}, ${short}, moe::${argtype}, ${desc}, ${deprname}, ${deprshortname})',
                        name=_encaps(opt.name), short=_encaps(
                            opt.short_name), argtype=opt.arg_vartype, desc=_encaps(opt.description),
                        deprname=_encaps_list(opt.deprecated_name), deprshortname=_encaps_list(
                            opt.deprecated_short_name)))
                self._writer.write_line('.setSources(moe::%s)' % (opt.source))
                if opt.hidden:
                    self._writer.write_line('.hidden()')
                if opt.redact:
                    self._writer.write_line('.redact()')
                for requires in opt.requires:
                    self._writer.write_line('.requires(%s)' % (_encaps(requires)))
                for conflicts in opt.conflicts:
                    self._writer.write_line('.incompatibleWith(%s)' % (_encaps(conflicts)))
                if opt.default is not None:
                    dflt = _encaps(opt.default) if opt.arg_vartype == "String" else opt.default
                    self._writer.write_line('.setDefault(moe::Value(%s))' % (dflt))
                if opt.implicit is not None:
                    impl = _encaps(opt.implicit) if opt.arg_vartype == "String" else opt.implicit
                    self._writer.write_line('.setImplicit(moe::Value(%s))' % (impl))
                if opt.duplicates_append:
                    self._writer.write_line('.composing()')
                if (opt.positional_start is not None) and (opt.positional_end is not None):
                    self._writer.write_line('.positional(%d, %d)' % (opt.positional_start,
                                                                     opt.positional_end))

                if opt.validator:
                    if opt.validator.callback:
                        self._writer.write_line(
                            common.template_args(
                                '.addConstraint(new moe::CallbackKeyConstraint<${argtype}>(${key}, ${callback}))',
                                argtype=vartype, key=_encaps(
                                    opt.name), callback=opt.validator.callback))

                    if (opt.validator.gt is not None) or (opt.validator.lt is not None) or (
                            opt.validator.gte is not None) or (opt.validator.lte is not None):
                        self._writer.write_line(
                            common.template_args(
                                '.addConstraint(new moe::BoundaryKeyConstraint<${argtype}>(${key}, ${gt}, ${lt}, ${gte}, ${lte}))',
                                argtype=vartype, key=_encaps(opt.name), gt='boost::none'
                                if opt.validator.gt is None else unicode(opt.validator.gt),
                                lt='boost::none' if opt.validator.lt is None else unicode(
                                    opt.validator.lt), gte='boost::none'
                                if opt.validator.gte is None else unicode(
                                    opt.validator.gte), lte='boost::none'
                                if opt.validator.lte is None else unicode(opt.validator.lte)))

        self.write_empty_line()

    def gen_config_options(self, spec):
        # type: (ast.IDLAST) -> None
        """Generate Config Option instances."""

        # pylint: disable=too-many-branches,too-many-statements

        has_storage_targets = False
        for opt in spec.configs:
            if opt.cpp_varname is not None:
                has_storage_targets = True
                if opt.cpp_vartype is not None:
                    with self._condition(opt.condition, preprocessor_only=True):
                        self._writer.write_line('%s %s;' % (opt.cpp_vartype, opt.cpp_varname))

        self.write_empty_line()

        root_opts = []  # type: List[ast.ConfigOption]
        sections = {}  # type: Dict[unicode, List[ast.ConfigOption]]
        for opt in spec.configs:
            if opt.section:
                try:
                    sections[opt.section].append(opt)
                except KeyError:
                    sections[opt.section] = [opt]
            else:
                root_opts.append(opt)

        with self.gen_namespace_block(''):
            # Group together options by section
            if spec.globals.configs and spec.globals.configs.initializer_name:
                blockname = spec.globals.configs.initializer_name
            else:
                blockname = 'idl_' + uuid.uuid4().hex

            with self._block('MONGO_MODULE_STARTUP_OPTIONS_REGISTER(%s)(InitializerContext*) {' %
                             (blockname), '}'):
                self._writer.write_line('namespace moe = ::mongo::optionenvironment;')
                self.write_empty_line()

                for opt in root_opts:
                    self.gen_config_option(opt, 'moe::startupOptions')

                for section_name, section_opts in sections.iteritems():
                    with self._block('{', '}'):
                        self._writer.write_line('moe::OptionSection section(%s);' %
                                                (_encaps(section_name)))
                        self.write_empty_line()

                        for opt in section_opts:
                            self.gen_config_option(opt, 'section')

                        self._writer.write_line(
                            'auto status = moe::startupOptions.addSection(section);')
                        with self._block('if (!status.isOK()) {', '}'):
                            self._writer.write_line('return status;')
                    self.write_empty_line()

                self._writer.write_line('return Status::OK();')
            self.write_empty_line()

            if has_storage_targets:
                # Setup initializer for storing configured options in their variables.
                with self._block('MONGO_STARTUP_OPTIONS_STORE(%s)(InitializerContext*) {' %
                                 (blockname), '}'):
                    self._writer.write_line('namespace moe = ::mongo::optionenvironment;')
                    self._writer.write_line('const auto& params = moe::startupOptionsParsed;')
                    self.write_empty_line()

                    for opt in spec.configs:
                        if opt.cpp_varname is None:
                            continue

                        vartype = ("moe::OptionTypeMap<moe::%s>::type" % (
                            opt.arg_vartype)) if opt.cpp_vartype is None else opt.cpp_vartype
                        with self._condition(opt.condition):
                            with self._block('if (params.count(%s)) {' % (_encaps(opt.name)), '}'):
                                self._writer.write_line('%s = params[%s].as<%s>();' %
                                                        (opt.cpp_varname, _encaps(opt.name),
                                                         vartype))
                        self.write_empty_line()

                    self._writer.write_line('return Status::OK();')

                self.write_empty_line()

        self.write_empty_line()

    def generate(self, spec, header_file_name):
        # type: (ast.IDLAST, unicode) -> None
        """Generate the C++ header to a stream."""
        self.gen_file_header()

        # Include platform/basic.h
        self.gen_include("mongo/platform/basic.h")
        self.write_empty_line()

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
            'mongo/db/command_generic_argument.h',
            'mongo/db/commands.h',
        ]

        if spec.server_parameters:
            header_list.append('mongo/idl/server_parameter.h')
            header_list.append('mongo/idl/server_parameter_with_storage.h')

        if spec.configs:
            header_list.append('mongo/util/options_parser/option_section.h')
            header_list.append('mongo/util/options_parser/startup_option_init.h')
            header_list.append('mongo/util/options_parser/startup_options.h')

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

                # Write field validators
                self.gen_field_validators(struct)
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

            if spec.server_parameters:
                self.gen_server_parameters(spec.server_parameters)
            if spec.configs:
                self.gen_config_options(spec)


def generate_header_str(spec):
    # type: (ast.IDLAST) -> unicode
    """Generate a C++ header in-memory."""
    stream = io.StringIO()
    text_writer = writer.IndentedTextWriter(stream)

    header = _CppHeaderFileWriter(text_writer)

    header.generate(spec)

    return stream.getvalue()


def _generate_header(spec, file_name):
    # type: (ast.IDLAST, unicode) -> None
    """Generate a C++ header."""

    str_value = generate_header_str(spec)

    # Generate structs
    with io.open(file_name, mode='wb') as file_handle:
        file_handle.write(str_value.encode())


def generate_source_str(spec, target_arch, header_file_name):
    # type: (ast.IDLAST, unicode, unicode) -> unicode
    """Generate a C++ source file in-memory."""
    stream = io.StringIO()
    text_writer = writer.IndentedTextWriter(stream)

    source = _CppSourceFileWriter(text_writer, target_arch)

    source.generate(spec, header_file_name)

    return stream.getvalue()


def _generate_source(spec, target_arch, file_name, header_file_name):
    # type: (ast.IDLAST, unicode, unicode, unicode) -> None
    """Generate a C++ source file."""
    str_value = generate_source_str(spec, target_arch, header_file_name)

    # Generate structs
    with io.open(file_name, mode='wb') as file_handle:
        file_handle.write(str_value.encode())


def generate_code(spec, target_arch, output_base_dir, header_file_name, source_file_name):
    # type: (ast.IDLAST, unicode, unicode, unicode, unicode) -> None
    """Generate a C++ header and source file from an idl.ast tree."""

    _generate_header(spec, header_file_name)

    if output_base_dir:
        include_h_file_name = os.path.relpath(
            os.path.normpath(header_file_name), os.path.normpath(output_base_dir))
    else:
        include_h_file_name = os.path.abspath(os.path.normpath(header_file_name))

    # Normalize to POSIX style for consistency across Windows and POSIX.
    include_h_file_name = include_h_file_name.replace("\\", "/")

    _generate_source(spec, target_arch, source_file_name, include_h_file_name)
