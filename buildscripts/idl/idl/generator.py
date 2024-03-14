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
"""IDL C++ Code Generator."""

import hashlib
import io
import itertools
import os
import re
import sys
import textwrap
from abc import ABCMeta, abstractmethod
from enum import Enum
from typing import Callable, Dict, List, Mapping, Optional, Tuple, Union, cast

from . import (ast, bson, common, cpp_types, enum_types, generic_field_list_types, struct_types,
               writer)


class _StructDataOwnership(Enum):
    """Enumerates the ways a struct may participate in ownership of it's data."""

    VIEW = 1  # Doesn't participate in ownership
    SHARED = 2  # Participates in non-exclusive ownership
    OWNER = 3  # Takes ownership of underlying data


def _get_field_member_name(field):
    # type: (ast.Field) -> str
    """Get the C++ class member name for a field."""
    return '_%s' % (common.camel_case(field.cpp_name))


def _get_field_member_setter_name(field):
    # type: (ast.Field) -> str
    """Get the C++ class setter name for a field."""
    return "set%s" % (common.title_case(field.cpp_name))


def _get_field_member_getter_name(field):
    # type: (ast.Field) -> str
    """Get the C++ class getter name for a field."""
    return "get%s" % (common.title_case(field.cpp_name))


def _get_has_field_member_name(field):
    # type: (ast.Field) -> str
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


def _get_field_kname(*args, **kwargs):
    """Get the C++ string constant name for a field."""
    if args:
        field_name = args[0].cpp_name
    else:
        field_name = kwargs["name"]
    return f'k{common.title_case(field_name)}'


def _get_field_enum(field):
    # type: (ast.Field) -> str
    return f'Field::{_get_field_kname(field)}'


def _get_field_constant_name(field):
    # type: (ast.Field) -> str
    """Get the C++ string constant name for a field."""
    return common.template_args('k${constant_name}FieldName',
                                constant_name=common.title_case(field.cpp_name))


def _get_field_member_validator_name(field):
    # type: (ast.Field) -> str
    """Get the name of the validator method for this field."""
    return 'validate%s' % common.title_case(field.cpp_name)


def _access_member(field):
    # type: (ast.Field) -> str
    """Get the declaration to access a member for a field."""
    member_name = _get_field_member_name(field)
    if field.optional:
        member_name = '(*%s)' % (member_name)
    return member_name


def _std_array_expr(value_type, elems):
    # type: (str, List[str]) -> str
    """Return a std::array<value_type, N>{elems} expression."""
    elem_str = ', '.join(elems)
    return f'std::array<{value_type}, {len(elems)}>{{{elem_str}}}'


def _get_bson_type_check(bson_element, ctxt_name, ast_type):
    # type: (str, str, ast.Type) -> str
    """Get the C++ bson type check for a Type."""
    # Deduplicate the types in the array.
    bson_types = list(set(ast_type.bson_serialization_type))
    if len(bson_types) == 1:
        if bson_types[0] in ['any', 'chain']:
            # Skip BSON validation for 'any' types since they are required to validate the
            # BSONElement.
            # Skip BSON validation for 'chain' types since they process the raw BSONObject the
            # encapsulating IDL struct parser is passed.
            return None

        if not bson_types[0] == 'bindata':
            return 'MONGO_likely(%s.checkAndAssertType(%s, %s))' % (
                ctxt_name, bson_element, bson.cpp_bson_type_name(bson_types[0]))
        return 'MONGO_likely(%s.checkAndAssertBinDataType(%s, %s))' % (
            ctxt_name, bson_element, bson.cpp_bindata_subtype_type_name(ast_type.bindata_subtype))
    else:
        return (
            f'MONGO_likely({ctxt_name}.checkAndAssertTypes({bson_element}, '
            f'{_std_array_expr("BSONType", [bson.cpp_bson_type_name(b) for b in bson_types])}))')


def _get_required_fields(struct):
    # type: (ast.Struct) -> List[ast.Field]
    return list(filter(_is_required_serializer_field, struct.fields))


def _get_all_fields(struct):
    # type: (ast.Struct) -> List[ast.Field]
    """Get a list of all the fields, including the command field."""
    all_fields = []
    if isinstance(struct, ast.Command) and struct.command_field:
        all_fields.append(struct.command_field)

    all_fields += struct.fields

    return sorted([field for field in all_fields], key=lambda f: f.cpp_name)


class _FieldUsageCheckerBase(object, metaclass=ABCMeta):
    """Check for duplicate fields, and required fields as needed."""

    def __init__(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Create a field usage checker."""
        self._writer = indented_writer  # type: writer.IndentedTextWriter
        self._fields = []  # type: List[ast.Field]

    @abstractmethod
    def add_store(self, field_name):
        # type: (str) -> None
        """Create the C++ field store initialization code."""
        pass

    @abstractmethod
    def add(self, field, bson_element_variable):
        # type: (ast.Field, str) -> None
        """Add a field to track."""
        pass

    @abstractmethod
    def add_final_checks(self):
        # type: () -> None
        """Output the code to check for missing fields."""
        pass


def _gen_field_usage_constant(field):
    # type: (ast.Field) -> str
    """Get the name for a bitset constant in field usage checking."""
    return "k%sBit" % (common.title_case(field.cpp_name))


def _gen_field_element_name(field):
    # type: (ast.Field) -> str
    """Get the name for a BSONElement pointer in field iteration."""
    return "BSONElement_%s" % (common.title_case(field.cpp_name))


def _gen_mark_present(field_name):
    # type: (str) -> str
    return f'_hasMembers.markPresent(static_cast<size_t>(RequiredFields::{field_name}));'


def _is_parse(field):
    # type: (ast.Field) -> bool
    """Returns true if `field` should participate in deserialization."""
    # Do not parse chained fields as fields since they are actually chained types.
    if field.chained and not field.chained_struct_field:
        return False
    # Internal only fields are not parsed from BSON objects
    if field.type and field.type.internal_only:
        return False
    return True


def _is_forwarding_disabled(field):
    # type: (ast.Field) -> bool
    """Returns true if forwarding is dieabled for `field`."""
    gfi = field.generic_field_info
    return not (gfi and gfi.get_should_forward())


def _get_constant(name):
    # type: (str) -> str
    """Transform an arbitrary label to a constant name."""
    return 'k' + re.sub(r'([^a-zA-Z0-9_]+)', '_', common.title_case(name))


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

        num_internal_only = len(
            [field.name for field in fields if field.type and field.type.internal_only])
        self.field_count = len(fields) - num_internal_only

        bit_id = 0
        for field in fields:
            if field.chained or (field.type and field.type.internal_only):
                continue

            self._writer.write_line(
                'const size_t %s = %d;' % (_gen_field_usage_constant(field), bit_id))
            bit_id += 1

        if bit_id != 0:
            self._writer.write_line('std::bitset<%d> usedFields;' % (self.field_count))

    def add_store(self, field_name):
        # type: (str) -> None
        """Create the C++ field store initialization code."""
        pass

    def add(self, field, bson_element_variable):
        # type: (ast.Field, str) -> None
        """Add a field to track."""
        if not field in self._fields:
            self._fields.append(field)

        with writer.IndentedScopedBlock(
                self._writer,
                'if (MONGO_unlikely(usedFields[%s])) {' % (_gen_field_usage_constant(field)), '}'):
            self._writer.write_line('ctxt.throwDuplicateField(%s);' % (bson_element_variable))
        self._writer.write_empty_line()

        self._writer.write_line('usedFields.set(%s);' % (_gen_field_usage_constant(field)))
        self._writer.write_empty_line()

        if field.stability == 'unstable':
            self._writer.write_line(
                'ctxt.checkAndthrowAPIStrictErrorIfApplicable(%s);' % (bson_element_variable))
            self._writer.write_empty_line()

    def add_final_checks(self):
        # type: () -> None
        """Output the code to check for missing fields."""
        required_fields = [
            field for field in self._fields
            if (not field.optional) and (not field.ignore) and (not field.default)
        ]

        # Don't output dead code we know is unused
        if len(required_fields) == 0:
            return

        # To build this bitmask, we assume less then 64 fields. If we exceed this count, we will need a new approach
        assert self.field_count < 64

        required_fields = sorted(required_fields, key=lambda f: f.cpp_name)

        bitmask = ' | '.join(
            ['(1ULL << %s)' % (_gen_field_usage_constant(rf)) for rf in required_fields])

        self._writer.write_line(f'constexpr std::uint64_t requiredFieldBitMask = {bitmask};')

        self._writer.write_line(
            'std::bitset<%d> requiredFields(requiredFieldBitMask);' % (self.field_count))

        self._writer.write_line(
            'bool hasMissingRequiredFields = (requiredFields & usedFields) != requiredFields;')

        with writer.IndentedScopedBlock(self._writer, 'if (hasMissingRequiredFields) {', '}'):
            for field in required_fields:
                # If 'field.default' is true, the fields(members) gets initialized with the default
                # value in the class definition. So, it's ok to skip setting the field to
                # default value here.
                with writer.IndentedScopedBlock(
                        self._writer, 'if (!usedFields[%s]) {' % (_gen_field_usage_constant(field)),
                        '}'):
                    self._writer.write_line(
                        'ctxt.throwMissingField(%s);' % (_get_field_constant_name(field)))


class _SlowFieldUsageChecker(_FastFieldUsageChecker):
    """
    Check for duplicate fields, and required fields as needed.

    Generates code with a C++ std::set to maintain a set of fields seen while parsing a BSON
    document. The std::set has O(N lg N) lookup, and allocates memory in the heap.
    The fast and slow duplicate/field usage checkers are merged together through
    inheritance. The fast checker assumes it only needs to check a finite list of
    fields for duplicates. The slow checker simply uses the fast check for all known
    fields and a std::set for other fields to detect duplication.
    """

    def __init__(self, indented_writer, fields):
        # type: (writer.IndentedTextWriter, List[ast.Field]) -> None
        super(_SlowFieldUsageChecker, self).__init__(indented_writer, fields)

        self._writer.write_line('std::set<StringData> usedFieldSet;')


def _get_field_usage_checker(indented_writer, struct):
    # type: (writer.IndentedTextWriter, ast.Struct) -> _FieldUsageCheckerBase

    # Only use the fast field usage checker if we never expect extra fields that we need to ignore
    # but still wish to do duplicate detection on.
    if struct.strict or struct.unsafe_dangerous_disable_extra_field_duplicate_checks:
        return _FastFieldUsageChecker(indented_writer, struct.fields)

    return _SlowFieldUsageChecker(indented_writer, struct.fields)


# Turn a python string into a C++ literal.
def _encaps(val):
    # type: (str) -> str
    if val is None:
        return '""'

    for srch, repl in {"\\": "\\\\", '"': '\\"', "'": "\\'", "\n": "\\n"}.items():
        if srch in val:
            val = val.replace(srch, repl)

    return '"' + val + '"'


# Turn a list of pything strings into a C++ initializer list.
def _encaps_list(vals):
    # type: (List[str]) -> str
    if vals is None:
        return '{}'

    return '{' + ', '.join([_encaps(v) for v in vals]) + '}'


# Translate an ast.Expression into C++ code.
def _get_expression(expr):
    # type: (ast.Expression) -> str
    if not expr.validate_constexpr:
        return expr.expr

    # Wrap in a lambda to let the compiler enforce constexprness for us.
    # The optimization pass should end up inlining it.
    return '([]{ constexpr auto value = %s; return value; })()' % expr.expr


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
        # type: (str) -> None
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
        # type: (str) -> None
        """Generate a system C++ include line."""
        self._writer.write_unindented_line('#include <%s>' % (include))

    def gen_include(self, include):
        # type: (str) -> None
        """Generate a non-system C++ include line."""
        self._writer.write_unindented_line('#include "%s"' % (include))

    def gen_namespace_block(self, namespace):
        # type: (str) -> writer.NamespaceScopeBlock
        """Generate a namespace block."""
        namespace_list = namespace.split("::")

        return writer.NamespaceScopeBlock(self._writer, namespace_list)

    def get_initializer_lambda(self, decl, unused=False, return_type=None):
        # type: (str, bool, str) -> writer.IndentedScopedBlock
        """Generate an indented block lambda initializing an outer scope variable."""
        prefix = '[[maybe_unused]] ' if unused else ''
        prefix = prefix + decl + ' = ([]'
        if return_type:
            prefix = prefix + '() -> ' + return_type
        return writer.IndentedScopedBlock(self._writer, prefix + ' {', '})();')

    def gen_description_comment(self, description):
        # type: (str) -> None
        """Generate a multiline comment with the description from the IDL."""
        self._writer.write_line("/**")
        for desc in description.split("\n"):
            self._writer.write_line(" * " + desc)
        self._writer.write_line(" */")

    def _with_template(self, template_params):
        # type: (Mapping[str,str]) -> writer.TemplateContext
        """Generate a template context for the current parameters."""
        return writer.TemplateContext(self._writer, template_params)

    def _block(self, opening, closing):
        # type: (str, str) -> Union[writer.IndentedScopedBlock,writer.EmptyBlock]
        """Generate an indented block if opening is not empty."""
        if not opening:
            return writer.EmptyBlock()

        return writer.IndentedScopedBlock(self._writer, opening, closing)

    def _predicate(self, check_str, use_else_if=False, constexpr=False):
        # type: (str, bool, bool) -> Union[writer.IndentedScopedBlock,writer.EmptyBlock]
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

    def _else(self, check_bool):
        # type: (bool) -> Union[writer.IndentedScopedBlock,writer.EmptyBlock]
        """Generate an else block if check_bool is true."""
        if not check_bool:
            return writer.EmptyBlock()

        return writer.IndentedScopedBlock(self._writer, 'else {', '}')

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
        # type: (str) -> writer.IndentedScopedBlock
        """Generate a class declaration block."""
        return writer.IndentedScopedBlock(self._writer,
                                          'class %s {' % common.title_case(class_name), '};')

    def gen_class_constructors(self, struct):
        # type: (ast.Struct) -> None
        """Generate the declarations for the class constructors."""
        struct_type_info = struct_types.get_struct_info(struct)

        constructor = struct_type_info.get_constructor_method(gen_header=True)
        self._writer.write_line(constructor.get_declaration())

        required_constructor = struct_type_info.get_required_constructor_method(gen_header=True)
        if len(required_constructor.args) != len(constructor.args):
            self._writer.write_line(required_constructor.get_declaration())

    def gen_field_list_entry_lookup_methods_struct(self, struct):
        # type: (ast.Struct) -> None
        """Generate the declarations for generic argument or reply field lookup methods."""
        info = generic_field_list_types.get_field_list_info(struct)
        self._writer.write_line(info.get_has_field_method().get_declaration())
        self._writer.write_line(info.get_should_forward_method().get_declaration())

    def gen_serializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate serializer method declarations."""
        struct_type_info = struct_types.get_struct_info(struct)
        self._writer.write_line(struct_type_info.get_serializer_method().get_declaration())

        maybe_op_msg_serializer = struct_type_info.get_op_msg_request_serializer_method()
        if maybe_op_msg_serializer:
            self._writer.write_line(maybe_op_msg_serializer.get_declaration())

        self._writer.write_line(struct_type_info.get_to_bson_method().get_declaration())

        self._writer.write_empty_line()

    def gen_deserializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate deserializer method declarations."""
        struct_type_info = struct_types.get_struct_info(struct)
        possible_deserializer_methods = [
            struct_type_info.get_deserializer_static_method(),
            struct_type_info.get_owned_deserializer_static_method(),
            struct_type_info.get_sharing_deserializer_static_method(),
            struct_type_info.get_op_msg_request_deserializer_static_method()
        ]
        for maybe_parse_method in possible_deserializer_methods:
            if maybe_parse_method:
                comment = maybe_parse_method.get_desc_for_comment()
                if comment:
                    self.gen_description_comment(comment)
                self._writer.write_line(maybe_parse_method.get_declaration())

        self._writer.write_empty_line()

    def gen_ownership_getter(self, struct):
        # type: (ast.Struct) -> None
        """Generate a getter that returns true if this IDL object owns its underlying data."""
        if struct.is_view:
            self.gen_description_comment(
                textwrap.dedent("""\
            *An IDL struct can either provide a view onto some underlying BSON data, or it can
            participate in owning that data. This function returns true if the struct participates in
            owning the underlying data.

            Note that the underlying data is not synchronized with the IDL struct over its lifetime; to
            generate a BSON representation of an IDL struct, use its `serialize` member functions.
            Participating in ownership of the underlying data merely allows the struct to ensure that
            struct members that are pointers-into-BSON (i.e. BSONElement and BSONObject) are valid for
            the lifetime of the struct itself."""))
            self._writer.write_line("bool isOwned() const { return _anchorObj.isOwned(); }")
        else:
            self.gen_description_comment(
                textwrap.dedent("""\
            This function will return true every time because there is no underlying BSONObj anchor.
            The object owns the data of all of its members."""))
            self._writer.write_line("bool isOwner() const { return true; }")

    def gen_protected_ownership_setters(self, struct):
        # type: (ast.Struct) -> None
        """Generate a setter that can be used to allow this IDL struct to particpate in the ownership of some BSON that the struct's members refer to."""
        if struct.is_view:
            # If the struct is not a view type, then a BSONObj anchor is not needed because we
            # know the struct owns all of its data.
            with self._block('void setAnchor(const BSONObj& obj) {', '}'):
                self._writer.write_line("invariant(obj.isOwned());")
                self._writer.write_line("_anchorObj = obj;")
            self._writer.write_empty_line()

            with self._block('void setAnchor(BSONObj&& obj) {', '}'):
                self._writer.write_line("invariant(obj.isOwned());")
                self._writer.write_line("_anchorObj = std::move(obj);")
            self._writer.write_empty_line()
        else:
            self._writer.write_line("void setAnchor(const BSONObj& obj) { }")
            self._writer.write_empty_line()
            self._writer.write_line("void setAnchor(BSONObj&& obj) { }")
            self._writer.write_empty_line()

    def gen_protected_serializer_methods(self, struct):
        # type: (ast.Struct) -> None
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
            'const_type': 'const ' if cpp_type_info.return_by_reference() else '',
        }

        # Generate a getter that disables xvalue for view types (i.e. StringData), constructed
        # optional types, and non-primitive types.
        with self._with_template(template_params):

            if field.chained_struct_field:
                self._writer.write_template(
                    '${const_type} ${param_type} ${method_name}() const { return %s.%s(); }' % (
                        (_get_field_member_name(field.chained_struct_field),
                         _get_field_member_getter_name(field))))

            elif field.type.is_struct:
                # Support mutable accessors
                self._writer.write_template(
                    'const ${param_type} ${method_name}() const { ${body} }')

                if not struct.immutable:
                    self._writer.write_template('${param_type} ${method_name}() { ${body} }')
            else:
                self._writer.write_template(
                    '${const_type}${param_type} ${method_name}() const { ${body} }')

                if field.non_const_getter:
                    self._writer.write_template('${param_type} ${method_name}() { ${body} }')

    def gen_validators(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ validators definition for a field."""
        assert field.validator

        cpp_type_info = cpp_types.get_cpp_type_without_optional(field)
        param_type = cpp_type_info.get_storage_type()

        if not cpp_types.is_primitive_type(param_type):
            param_type = 'const ' + param_type + '&'

        template_params = {
            'method_name': _get_field_member_validator_name(field),
            'param_type': param_type,
        }

        with self._with_template(template_params):
            # Declare method implemented in C++ file.
            self._writer.write_template('void ${method_name}(${param_type} value);')
            self._writer.write_template(
                'void ${method_name}(IDLParserContext& ctxt, ${param_type} value);')

        self._writer.write_empty_line()

    def gen_setter(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ setter definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        param_type = cpp_type_info.get_getter_setter_type()
        is_serial = _is_required_serializer_field(field)
        memfn = _get_field_member_setter_name(field)
        if field.chained_struct_field:
            body = "{}.{}(std::move(value));".format(
                _get_field_member_name(field.chained_struct_field),
                _get_field_member_setter_name(field))
        else:
            body = cpp_type_info.get_setter_body(
                _get_field_member_name(field),
                _get_field_member_validator_name(field) if field.validator is not None else '')
        set_has = _gen_mark_present(field.cpp_name) if is_serial else ''

        with self._block(f'void {memfn}({param_type} value) {{', '}'):
            self._writer.write_line(f'{body}')
            if (set_has):
                self._writer.write_line(set_has)
        self._writer.write_empty_line()

    def gen_constexpr_getters(self):
        # type: () -> None
        """Generate the getters for constexpr data."""
        self._writer.write_line(
            'constexpr bool getIsCommandReply() const { return _isCommandReply; }')

    def gen_member(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ class member definition for a field."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        member_type = cpp_type_info.get_storage_type()
        member_name = _get_field_member_name(field)

        # Struct fields are allowed to specify default: true so that the member gets default-
        # constructed.
        if field.default and not field.constructed:
            if field.type.is_enum:
                self._writer.write_line('%s %s{%s::%s};' % (member_type, member_name,
                                                            field.type.cpp_type, field.default))
            elif field.type.is_struct:
                self._writer.write_line('%s %s;' % (member_type, member_name))
            else:
                self._writer.write_line('%s %s{%s};' % (member_type, member_name, field.default))
        else:
            self._writer.write_line('%s %s;' % (member_type, member_name))

    def gen_constexpr_members(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ class member definition for constexpr data."""
        cpp_string_val = "true" if struct.is_command_reply else "false"
        self._writer.write_line(f'static constexpr bool _isCommandReply{{{cpp_string_val}}};')

    def gen_serializer_member(self, field):
        # type: (ast.Field) -> None
        """Generate the C++ class bool has_<field> member definition for a field."""
        has_member_name = _get_has_field_member_name(field)

        # Use a bitfield to save space
        self._writer.write_line('bool %s : 1;' % (has_member_name))

    def gen_string_constants_declarations(self, struct):
        # type: (ast.Struct) -> None
        """Generate a StringData constant for field name."""

        fields = [
            field for field in _get_all_fields(struct)
            if not field.type or (field.type and not field.type.internal_only)
        ]
        for field in fields:
            self._writer.write_line(
                common.template_args('static constexpr auto ${constant_name} = "${field_name}"_sd;',
                                     constant_name=_get_field_constant_name(field),
                                     field_name=field.name))

        if isinstance(struct, ast.Command):
            self._writer.write_line(
                common.template_args(
                    'static constexpr auto kCommandDescription = ${description}_sd;',
                    description=_encaps(struct.description)))

            self._writer.write_line(
                common.template_args('static constexpr auto kCommandName = "${command_name}"_sd;',
                                     command_name=struct.command_name))

            # Initialize constexpr for command alias if specified in the IDL spec.
            if struct.command_alias:
                self._writer.write_line(
                    common.template_args(
                        'static constexpr auto kCommandAlias = "${command_alias}"_sd;',
                        command_alias=struct.command_alias))

    def gen_field_enum(self, struct):
        # type: (ast.Struct) -> None
        """Declare the public enum and string constants for struct fields."""
        with self._block('enum class Field {', '};'):
            for f in struct.fields:
                self._writer.write_line(f'{_get_field_kname(f)},')
        with self._block('static constexpr std::array fieldNames{', '};'):
            for f in struct.fields:
                self._writer.write_line(f'"{f.name}"_sd,')
        self._writer.write_empty_line()

    def gen_required_field_enum(self, struct):
        self._writer.write_line('enum class RequiredFields : size_t { %s };' % ', '.join(
            [f.cpp_name for f in _get_required_fields(struct)]))

    def gen_authorization_contract_declaration(self, struct):
        # type: (ast.Struct) -> None
        """Generate the authorization contract declaration."""

        if not isinstance(struct, ast.Command):
            return

        if struct.access_checks is None:
            return

        self._writer.write_line('static AuthorizationContract kAuthorizationContract;')
        self.write_empty_line()

    def gen_enum_functions(self, idl_enum):
        # type: (ast.Enum) -> None
        """Generate the declaration for an enum's supporting functions."""
        enum_type_info = enum_types.get_type_info(idl_enum)

        self._writer.write_line("%s;" % (enum_type_info.get_deserializer_declaration()))

        self._writer.write_line("%s;" % (enum_type_info.get_serializer_declaration()))

        extra_data_decl = enum_type_info.get_extra_data_declaration()
        if extra_data_decl is not None:
            self._writer.write_line("%s;" % (extra_data_decl))

    def gen_enum_declaration(self, idl_enum):
        # type: (ast.Enum) -> None
        """Generate the declaration for an enum."""
        enum_type_info = enum_types.get_type_info(idl_enum)

        with self._block('enum class %s : std::int32_t {' % (enum_type_info.get_cpp_type_name()),
                         '};'):
            for enum_value in idl_enum.values:
                if enum_value.description is not None:
                    self.gen_description_comment(enum_value.description)
                self._writer.write_line(
                    common.template_args('${name}${value},', name=enum_value.name,
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

    def gen_field_list_entries_declaration_struct(self, struct):
        # type: (ast.Struct) -> None
        """Generate the field list entries map for a generic argument or reply field list."""
        field_list_info = generic_field_list_types.get_field_list_info(struct)
        self._writer.write_line(
            common.template_args('// Map: fieldName -> ${should_forward_name}',
                                 should_forward_name=field_list_info.get_should_forward_name()))
        self._writer.write_line("static const StaticImmortal<StringMap<bool>> _genericFields;")
        self.write_empty_line()

    def gen_known_fields_declaration(self):
        # type: () -> None
        """Generate all the known fields vectors for a command."""
        self._writer.write_line("static const std::vector<StringData> _knownBSONFields;")
        self._writer.write_line("static const std::vector<StringData> _knownOP_MSGFields;")
        self.write_empty_line()

    def gen_comparison_operators_declarations(self, struct):
        # type: (ast.Struct) -> None
        """Generate comparison operators declarations for the type."""

        with self._block("auto _relopTuple() const {", "}"):
            sorted_fields = sorted([
                field for field in struct.fields
                if (not field.ignore and not (field.type and field.type.internal_only))
                and field.comparison_order != -1
            ], key=lambda f: f.comparison_order)
            self._writer.write_line("return std::tuple({});".format(", ".join(
                map(lambda f: "idl::relop::Ordering{{{}}}".format(_get_field_member_name(f)),
                    sorted_fields))))

        for op in ['==', '!=', '<', '>', '<=', '>=']:
            with self._block(
                    common.template_args(
                        "friend bool operator${op}(const ${cls}& a, const ${cls}& b) {", op=op,
                        cls=common.title_case(struct.name)), "}"):
                self._writer.write_line(
                    common.template_args('return a._relopTuple() ${op} b._relopTuple();', op=op))

        self.write_empty_line()

    def _gen_exported_constexpr(self, name, suffix, expr, condition):
        # type: (str, str, ast.Expression, ast.Condition) -> None
        """Generate exports for default initializer."""
        if not (name and expr and expr.export):
            return

        with self._condition(condition, preprocessor_only=True):
            self._writer.write_line(
                'constexpr auto %s%s = %s;' % (_get_constant(name), suffix, expr.expr))

        self.write_empty_line()

    def _gen_extern_declaration(self, vartype, varname, condition):
        # type: (str, str, ast.Condition) -> None
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

    def _gen_config_function_declaration(self, spec):
        # type: (ast.IDLAST) -> None
        """Generate function declarations for config initializers."""

        initializer = spec.globals.configs and spec.globals.configs.initializer
        if not initializer:
            return

        if initializer.register:
            self._writer.write_line(
                'Status %s(optionenvironment::OptionSection*);' % (initializer.register))
        if initializer.store:
            self._writer.write_line(
                'Status %s(const optionenvironment::Environment&);' % (initializer.store))

        if initializer.register or initializer.store:
            self.write_empty_line()

    def gen_server_parameter_class(self, scp):
        # type: (ast.ServerParameter) -> None
        """Generate a C++ class definition for a ServerParameter."""
        if scp.cpp_class is None:
            return

        cls = scp.cpp_class

        with self._block('class %s : public ServerParameter {' % (cls.name), '};'):
            self._writer.write_unindented_line('public:')
            if scp.default is not None:
                self._writer.write_line(
                    'static constexpr auto kDataDefault = %s;' % (scp.default.expr))

            if cls.override_ctor:
                # Explicit custom constructor.
                self._writer.write_line(cls.name + '(StringData name, ServerParameterType spt);')
            else:
                # Inherit base constructor.
                self._writer.write_line('using ServerParameter::ServerParameter;')
            self.write_empty_line()

            self._writer.write_line(
                'void append(OperationContext*, BSONObjBuilder*, StringData, const boost::optional<TenantId>&) final;'
            )
            if cls.override_set:
                self._writer.write_line(
                    'Status set(const BSONElement&, const boost::optional<TenantId>&) final;')
            self._writer.write_line(
                'Status setFromString(StringData, const boost::optional<TenantId>&) final;')

            # If override_validate is set, provide an override definition. Otherwise, it will inherit
            # from the base ServerParameter implementation.
            if cls.override_validate:
                self._writer.write_line(
                    'Status validate(const BSONElement&, const boost::optional<TenantId>& tenantId) const final;'
                )

            # The reset() and getClusterParameterTime() methods must be custom implemented for
            # specialized cluster server parameters. Provide the declarations here.
            if scp.set_at == 'ServerParameterType::kClusterWide':
                self._writer.write_line('Status reset(const boost::optional<TenantId>&) final;')
                self._writer.write_line(
                    'LogicalTime getClusterParameterTime(const boost::optional<TenantId>&) const final;'
                )

            if cls.data is not None:
                self.write_empty_line()
                if scp.default is not None:
                    self._writer.write_line('%s _data{kDataDefault};' % (cls.data))
                else:
                    self._writer.write_line('%s _data;' % (cls.data))

        self.write_empty_line()

    def gen_template_declaration(self):
        # type: () -> None
        """Generate a template declaration for a command's base class."""
        self._writer.write_line('template <typename Derived>')

    def gen_derived_class_declaration_block(self, class_name):
        # type: (str) -> writer.IndentedScopedBlock
        """Generate a command's base class declaration block."""
        return writer.IndentedScopedBlock(
            self._writer, 'class %s : public TypedCommand<Derived> {' % class_name, '};')

    def gen_type_alias_declaration(self, new_type_name, old_type_name):
        # type: (str, str) -> None
        """Generate a type alias declaration."""
        self._writer.write_line(
            'using %s = %s;' % (new_type_name, common.title_case(old_type_name)))

    def gen_derived_class_constructor(self, command_name, api_version, base_class,
                                      *base_class_args):
        # type: (str, str, str, *str) -> None
        """Generate a derived class constructor."""
        class_name = common.title_case(command_name) + "CmdVersion" + api_version + "Gen"
        args = ", ".join(base_class_args)
        self._writer.write_line('%s(): %s(%s) {}' % (class_name, base_class, args))

    def gen_derived_class_destructor(self, command_name, api_version):
        # type: (str, str) -> None
        """Generate a derived class destructor."""
        class_name = common.title_case(command_name) + "CmdVersion" + api_version + "Gen"
        self._writer.write_line('virtual ~%s() = default;' % (class_name))

    def gen_api_version_fn(self, is_api_versions, api_version):
        # type: (bool, Union[str, bool]) -> None
        """Generate an apiVersions or deprecatedApiVersions function for a command's base class."""
        fn_name = "apiVersions" if is_api_versions else "deprecatedApiVersions"
        fn_def = 'virtual const std::set<std::string>& %s() const final' % fn_name
        value = "kApiVersions1" if api_version else "kNoApiVersions"
        with self._block('%s {' % (fn_def), '}'):
            self._writer.write_line('return %s;' % value)

    def gen_invocation_base_class_declaration(self, command):
        # type: (ast.Command) -> None
        """Generate the InvocationBaseGen class for a command's base class."""
        class_declaration = 'class InvocationBaseGen : public _TypedCommandInvocationBase {'
        with writer.IndentedScopedBlock(self._writer, class_declaration, '};'):
            # public requires special indentation that aligns with the class definition.
            self._writer.unindent()
            self._writer.write_line('public:')
            self._writer.indent()

            # Inherit base constructor.
            self._writer.write_line(
                'using _TypedCommandInvocationBase::_TypedCommandInvocationBase;')

            self._writer.write_line('virtual Reply typedRun(OperationContext* opCtx) = 0;')

            if command.access_checks == []:
                self._writer.write_line(
                    'void doCheckAuthorization(OperationContext* opCtx) const final {}')

    def generate_versioned_command_base_class(self, command):
        # type: (ast.Command) -> None
        """Generate a command's C++ base class to a stream."""
        class_name = "%sCmdVersion%sGen" % (common.title_case(command.command_name),
                                            command.api_version)

        self.write_empty_line()

        self.gen_template_declaration()
        with self.gen_derived_class_declaration_block(class_name):
            # Write type alias for InvocationBase.
            self.gen_type_alias_declaration('_TypedCommandInvocationBase',
                                            'typename TypedCommand<Derived>::InvocationBase')

            self.write_empty_line()

            self.write_unindented_line('public:')

            # Write type aliases for Request and Reply.
            self.gen_type_alias_declaration("Request", command.cpp_name)
            self.gen_type_alias_declaration("Reply", command.reply_type.type.cpp_type)

            # Generate a constructor for generated derived class if command alias is specified.
            if command.command_alias:
                self.write_empty_line()
                self.gen_derived_class_constructor(command.command_name, command.api_version,
                                                   'TypedCommand<Derived>', 'Request::kCommandName',
                                                   'Request::kCommandAlias')

            self.write_empty_line()

            # Generate a destructor for generated derived class.
            self.gen_derived_class_destructor(command.command_name, command.api_version)

            self.write_empty_line()

            # Write apiVersions() and deprecatedApiVersions() functions.
            self.gen_api_version_fn(True, command.api_version)
            self.gen_api_version_fn(False, command.is_deprecated)

            # Wrte authorization contract code
            if command.access_checks is not None:
                self._writer.write_line(
                    'const AuthorizationContract* getAuthorizationContract() const final { return &Request::kAuthorizationContract; } '
                )
                self.write_empty_line()

            # Write InvocationBaseGen class.
            self.gen_invocation_base_class_declaration(command)

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
            'mongo/bson/simple_bsonobj_comparator.h',
            'mongo/idl/idl_parser.h',
            'mongo/rpc/op_msg.h',
            'mongo/stdx/unordered_map.h',
            'mongo/util/decimal_counter.h',
            'mongo/util/serialization_context.h',
        ] + spec.globals.cpp_includes

        if spec.configs:
            header_list.append('mongo/util/options_parser/option_description.h')
            config_init = spec.globals.configs and spec.globals.configs.initializer
            if config_init and (config_init.register or config_init.store):
                header_list.append('mongo/util/options_parser/option_section.h')
                header_list.append('mongo/util/options_parser/environment.h')

        if spec.server_parameters:
            if [
                    param for param in spec.server_parameters
                    if param.feature_flag or (param.condition and param.condition.feature_flag)
            ]:
                header_list.append('mongo/db/feature_flag.h')
            if [
                    param for param in spec.server_parameters
                    if param.condition and param.condition.min_fcv
            ]:
                header_list.append('mongo/db/feature_compatibility_version_parser.h')
            header_list.append('mongo/db/server_parameter.h')
            header_list.append('mongo/db/server_parameter_with_storage.h')

        # Include this for TypedCommand only if a base class will be generated for a command in this
        # file.
        if any(command.api_version for command in spec.commands):
            header_list.append('mongo/db/commands.h')

        # Include serialization options only if there is a struct which is part of a query shape.
        if any(struct.query_shape_component for struct in spec.structs):
            header_list.append('mongo/db/query/query_shape/serialization_options.h')

        header_list.sort()

        for include in header_list:
            self.gen_include(include)

        self.write_empty_line()

        self._writer.write_line("namespace mongo { class AuthorizationContract; }")
        self.write_empty_line()

        # Generate namespace
        with self.gen_namespace_block(spec.globals.cpp_namespace):
            self.write_empty_line()

            for idl_enum in spec.enums:
                self.gen_description_comment(idl_enum.description)
                self.gen_enum_declaration(idl_enum)
                self._writer.write_empty_line()

                self.gen_enum_functions(idl_enum)
                self._writer.write_empty_line()

            all_structs = spec.structs + cast(List[ast.Struct], spec.commands)

            for struct in all_structs:
                self.gen_description_comment(struct.description)
                with self.gen_class_declaration_block(struct.cpp_name):
                    self.write_unindented_line('public:')
                    self.gen_field_enum(struct)

                    if isinstance(struct, ast.Command):
                        if struct.reply_type:
                            # Alias the reply type as a named type for commands
                            self.gen_type_alias_declaration("Reply",
                                                            struct.reply_type.type.cpp_type)
                        else:
                            self._writer.write_line('using Reply = void;')

                    # Generate a sorted list of string constants
                    self.gen_string_constants_declarations(struct)
                    self.write_empty_line()

                    self.gen_authorization_contract_declaration(struct)

                    # Write constructor
                    self.gen_class_constructors(struct)
                    self.write_empty_line()

                    # Write serialization
                    self.gen_serializer_methods(struct)

                    # Write deserialization
                    self.gen_deserializer_methods(struct)

                    if isinstance(struct, ast.Command):
                        self.gen_op_msg_request_methods(struct)

                    self.gen_ownership_getter(struct)

                    # Write getters & setters
                    for field in struct.fields:
                        if not field.ignore:
                            if field.description:
                                self.gen_description_comment(field.description)
                            self.gen_getter(struct, field)
                            if not struct.immutable or (field.type and field.type.internal_only):
                                self.gen_setter(field)

                    # Generate getters for any constexpr/compile-time struct data
                    self.write_empty_line()
                    self.gen_constexpr_getters()

                    if struct.generic_list_type:
                        self.gen_field_list_entry_lookup_methods_struct(struct)

                    self.write_unindented_line('protected:')
                    if (struct.is_view):
                        # If the struct is not a view type, then a BSONObj anchor is not needed because we
                        # know the struct owns all of its data.
                        self.gen_protected_ownership_setters(struct)
                        self._writer.write_line("BSONObj _anchorObj;")

                    self.gen_protected_serializer_methods(struct)

                    # Write private validators
                    if [field for field in struct.fields if field.validator]:
                        self.write_unindented_line('private:')
                        for field in struct.fields:
                            if not field.ignore and not struct.immutable and field.validator:
                                self.gen_validators(field)

                    self.write_unindented_line('private:')
                    self._writer.write_line('struct FieldInfo;')

                    self.gen_required_field_enum(struct)
                    self.write_empty_line()

                    if struct.generate_comparison_operators:
                        self.gen_comparison_operators_declarations(struct)

                    # declare internal_only fields first
                    for field in struct.fields:
                        if field.type and field.type.internal_only:
                            self.gen_member(field)

                    # Write command member variables
                    if isinstance(struct, ast.Command):
                        self.gen_known_fields_declaration()
                        self.write_empty_line()

                        self.gen_op_msg_request_member(struct)

                    # Write member variables
                    for field in struct.fields:
                        if not field.ignore and not field.chained_struct_field:
                            if not (field.type and field.type.internal_only):
                                self.gen_member(field)

                    # Write the HasMember member variable. Note that this is a noop class in
                    # non-debug builds, and is marked MONGO_COMPILER_NO_UNIQUE_ADDRESS so that the
                    # compiler knows that it should be optimized to take up no space when possible.
                    self._writer.write_line(
                        'MONGO_COMPILER_NO_UNIQUE_ADDRESS mongo::idl::HasMembers<%s> _hasMembers;' %
                        len(_get_required_fields(struct)))
                    # Write constexpr struct data
                    self.gen_constexpr_members(struct)

                self.write_empty_line()

            for scp in spec.server_parameters:
                if scp.cpp_class is None:
                    self._gen_exported_constexpr(scp.name, 'Default', scp.default, scp.condition)
                self._gen_extern_declaration(scp.cpp_vartype, scp.cpp_varname, scp.condition)
                self.gen_server_parameter_class(scp)

            if spec.configs:
                for opt in spec.configs:
                    self._gen_exported_constexpr(opt.name, 'Default', opt.default, opt.condition)
                    self._gen_extern_declaration(opt.cpp_vartype, opt.cpp_varname, opt.condition)
                self._gen_config_function_declaration(spec)

            # Write a base class for each command in API Version 1.
            for command in spec.commands:
                if command.api_version:
                    self.generate_versioned_command_base_class(command)

        # Specialize `mongo::idlEnumCount<E>` for each enum `E``.
        with self.gen_namespace_block("mongo"):
            for idl_enum in spec.enums:
                enum_type_info = enum_types.get_type_info(idl_enum)
                cpp_namespace = idl_enum.cpp_namespace
                cpp_name = enum_type_info.get_cpp_type_name()
                full_cpp_name = "::{}::{}".format(cpp_namespace, cpp_name)
                self._writer.write_line("template<> constexpr inline size_t idlEnumCount<%s> = %d;"
                                        % (full_cpp_name, len(idl_enum.values)))


class _CppSourceFileWriter(_CppFileWriterBase):
    """C++ .cpp File writer."""

    _EMPTY_TENANT = "boost::optional<mongo::TenantId>{}"

    def __init__(self, indented_writer, target_arch):
        # type: (writer.IndentedTextWriter, str) -> None
        """Create a C++ .cpp file code writer."""
        self._target_arch = target_arch
        super(_CppSourceFileWriter, self).__init__(indented_writer)

    def gen_field_info(self, struct):
        # type: (ast.Struct) -> None
        """Generates a definition of struct's `FieldInfo` nested type.

        It contains static metadata of struct's members.
        """
        cls = common.title_case(struct.cpp_name)
        with self._block(f'struct {cls}::FieldInfo {{', '};'):
            for func, pred in (
                ('findStructField', lambda f: True),
                ('findRequiredField', _is_required_serializer_field),
                ('findForwardingDisabledField', _is_forwarding_disabled),
                ('findParsedField', _is_parse),
            ):
                selected_fields = [x for x in filter(pred, struct.fields)]
                self._writer.write_line('template <typename OnMatch, typename OnFail>')
                with self._block(
                        f'static auto {func}(StringData s, const OnMatch& onMatch, const OnFail& onFail) {{',
                        '};'):
                    if len(selected_fields) == 0:
                        self._writer.write_line('return onFail();')
                    else:
                        with self._block('static constexpr auto adaptMatch = [](int i) {', '};'):
                            with self._block('static constexpr auto arr = std::to_array<Field>({',
                                             '});'):
                                for f in selected_fields:
                                    self._writer.write_line(f'{_get_field_enum(f)},')
                            self._writer.write_line('return arr[i];')
                        writer.gen_string_table_find_function_block(
                            self._writer, 's', 'onMatch(adaptMatch({}))', 'onFail()',
                            [f.name for f in selected_fields])

    def _gen_field_deserializer_expression(self, element_name, field, ast_type, tenant):
        # type: (str, ast.Field, ast.Type, str) -> str
        """
        Generate the C++ deserializer piece for a field.

        Writes multiple lines into the generated file.
        Returns the final statement to access the deserialized value.
        """
        serialization_context = "getSerializationContext()"
        if ast_type.is_struct:
            validated_tenancy_scope = 'ctxt.getValidatedTenancyScope()'
            if 'request' in tenant:
                validated_tenancy_scope = 'request.validatedTenancyScope'
            self._writer.write_line('IDLParserContext tempContext(%s, &ctxt, %s, %s, %s);' %
                                    (_get_field_constant_name(field), validated_tenancy_scope,
                                     serialization_context, tenant))
            self._writer.write_line('const auto localObject = %s.Obj();' % (element_name))
            return '%s::parse(tempContext, localObject)' % (ast_type.cpp_type, )
        elif ast_type.deserializer and 'BSONElement::' in ast_type.deserializer:
            method_name = writer.get_method_name(ast_type.deserializer)
            return '%s.%s()' % (element_name, method_name)

        assert not ast_type.is_variant

        # Custom method, call the method on object.
        bson_cpp_type = cpp_types.get_bson_cpp_type(ast_type)

        if bson_cpp_type:
            # Call a static class method with the signature:
            # Class Class::method(StringData value)
            # or
            # Class::method(const BSONObj& value)
            expression = bson_cpp_type.gen_deserializer_expression(self._writer, element_name)
            if ast_type.deserializer:
                method_name = writer.get_method_name_from_qualified_method_name(
                    ast_type.deserializer)

                # For fields which are enums, pass a IDLParserContext
                if ast_type.is_enum:
                    validated_tenancy_scope = 'ctxt.getValidatedTenancyScope()'
                    if 'request' in tenant:
                        validated_tenancy_scope = 'request.validatedTenancyScope'
                    self._writer.write_line(
                        'IDLParserContext tempContext(%s, &ctxt, %s, %s, %s);' %
                        (_get_field_constant_name(field), validated_tenancy_scope,
                         serialization_context, tenant))
                    return common.template_args("${method_name}(tempContext, ${expression})",
                                                method_name=method_name, expression=expression)

                if ast_type.deserialize_with_tenant:
                    return common.template_args(
                        "${method_name}(${tenant}, ${expression}, ${context})",
                        method_name=method_name, tenant=tenant, expression=expression,
                        context=serialization_context)
                else:
                    return common.template_args("${method_name}(${expression})",
                                                method_name=method_name, expression=expression)

            # BSONObjects are allowed to be pass through without deserialization
            assert ast_type.bson_serialization_type in [['object'], ['array']]
            return expression

        # Call a static class method with the signature:
        # Class Class::method(const BSONElement& value)
        method_name = writer.get_method_name_from_qualified_method_name(ast_type.deserializer)

        if ast_type.deserialize_with_tenant:
            return '%s(%s, %s, %s)' % (method_name, tenant, element_name, serialization_context)
        else:
            return '%s(%s)' % (method_name, element_name)

    def _gen_array_deserializer(self, field, bson_element, ast_type, tenant):
        # type: (ast.Field, str, ast.Type, str) -> None
        """Generate the C++ deserializer piece for an array field."""
        assert ast_type.is_array
        cpp_type_info = cpp_types.get_cpp_type_from_cpp_type_name(field, ast_type.cpp_type, True)
        cpp_type = cpp_type_info.get_type_name()

        self._writer.write_line('DecimalCounter<std::uint32_t> expectedFieldNumber{0};')
        validated_tenancy_scope = 'ctxt.getValidatedTenancyScope()'
        if 'request' in tenant:
            validated_tenancy_scope = 'request.validatedTenancyScope'
        self._writer.write_line('const IDLParserContext arrayCtxt(%s, &ctxt, %s, %s, %s);' %
                                (_get_field_constant_name(field), validated_tenancy_scope,
                                 "getSerializationContext()", tenant))
        self._writer.write_line('std::vector<%s> values;' % (cpp_type))
        self._writer.write_empty_line()

        self._writer.write_line('const BSONObj arrayObject = %s.Obj();' % (bson_element))

        with self._block('for (const auto& arrayElement : arrayObject) {', '}'):

            self._writer.write_line(
                'const auto arrayFieldName = arrayElement.fieldNameStringData();')

            with self._predicate('MONGO_likely(arrayFieldName == expectedFieldNumber)'):
                check = _get_bson_type_check('arrayElement', 'arrayCtxt', ast_type)

                with self._predicate(check):
                    if ast_type.is_variant:
                        # _gen_variant_deserializer generates code to parse the variant into the variable "_" + field.cpp_name,
                        # so we create a local variable '_tmp'
                        # and change cpp_name (for the duration of the _gen_variant_deserializer call) to 'tmp' so we can pass '_tmp'
                        # to values.push_back below.
                        self._writer.write_line('%s _tmp;' % ast_type.cpp_type)
                        cpp_name = field.cpp_name
                        field.cpp_name = 'tmp'
                        array_value = self._gen_variant_deserializer(field, 'arrayElement', tenant)
                        field.cpp_name = cpp_name
                        self._writer.write_line('values.push_back(std::move(%s));' % (array_value))
                    else:
                        array_value = self._gen_field_deserializer_expression(
                            'arrayElement', field, ast_type, tenant)
                        self._writer.write_line('values.push_back(%s);' % (array_value))

            with self._block('else {', '}'):
                self._writer.write_line(
                    'arrayCtxt.throwBadArrayFieldNumberSequence(arrayFieldName, expectedFieldNumber);'
                )

            self._writer.write_line('++expectedFieldNumber;')

        if field.validator:
            self._writer.write_line('%s(values);' % (_get_field_member_validator_name(field)))

        if field.chained_struct_field:
            if field.type.is_variant:
                self._writer.write_line('%s.%s(%s(std::move(values)));' %
                                        (_get_field_member_name(field.chained_struct_field),
                                         _get_field_member_setter_name(field), field.type.cpp_type))
            else:
                self._writer.write_line('%s.%s(std::move(values));' % (_get_field_member_name(
                    field.chained_struct_field), _get_field_member_setter_name(field)))
        else:
            self._writer.write_line('%s = std::move(values);' % (_get_field_member_name(field)))

    def _gen_variant_deserializer(self, field, bson_element, tenant):
        # type: (ast.Field, str, str) -> str
        """Generate the C++ deserializer piece for a variant field."""
        self._writer.write_empty_line()
        self._writer.write_line('const BSONType variantType = %s.type();' % (bson_element, ))

        array_types = [v for v in field.type.variant_types if v.is_array]
        scalar_types = [v for v in field.type.variant_types if not v.is_array]

        self._writer.write_line('switch (variantType) {')
        if array_types:
            self._writer.write_line('case Array:')
            with self._block('{', '}'):
                # If the array is empty, we can't infer its element type. Use the first
                # array type as a fallback to cover that case.
                fallback_type = array_types[0].bson_serialization_type[0]
                fallback_bson_cpp_type = bson.cpp_bson_type_name(fallback_type)
                condition = '%s.Obj().isEmpty()' % (bson_element, )
                self._writer.write_line(
                    'const BSONType elemType = %s ? %s : %s.Obj().firstElement().type();' %
                    (condition, fallback_bson_cpp_type, bson_element))

                # Start inner switch statement, for each type the first element could be.
                self._writer.write_line('switch (elemType) {')
                for array_type in array_types:
                    for bson_type in array_type.bson_serialization_type:
                        self._writer.write_line('case %s:' % (bson.cpp_bson_type_name(bson_type), ))
                    # Each copy of the array deserialization code gets an anonymous block.
                    with self._block('{', '}'):
                        self._gen_array_deserializer(field, bson_element, array_type, tenant)
                        self._writer.write_line('break;')

                self._writer.write_line('default:')
                self._writer.indent()
                expected_types = [
                    bson.cpp_bson_type_name(t.bson_serialization_type[0]) for t in array_types
                ]
                self._writer.write_line(
                    f'ctxt.throwBadType({bson_element},  {_std_array_expr("BSONType", expected_types)});'
                )
                self._writer.write_line('break;')
                self._writer.unindent()
                # End of inner switch.
                self._writer.write_line('}')

                # End of "case Array:".
                self._writer.write_line('break;')

        for scalar_type in scalar_types:
            for bson_type in scalar_type.bson_serialization_type:
                self._writer.write_line('case %s:' % (bson.cpp_bson_type_name(bson_type), ))
                with self._block('{', '}'):
                    self.gen_field_deserializer(field, scalar_type, "bsonObject", bson_element,
                                                None, tenant, check_type=False)
                    self._writer.write_line('break;')

        if field.type.variant_struct_types:
            with self._block('case Object: {', '} break;'):
                self._gen_variant_deserializer_from_obj(field,
                                                        field_name=_get_field_member_name(field),
                                                        bson_element='%s.Obj()' % bson_element)

        self._writer.write_line('default:')
        self._writer.indent()
        expected_types = [
            bson.cpp_bson_type_name(t.bson_serialization_type[0]) for t in array_types
        ]
        self._writer.write_line(f'ctxt.throwBadType({bson_element}, '
                                f'{_std_array_expr("BSONType", expected_types)});')
        self._writer.write_line('break;')
        self._writer.unindent()

        # End of outer switch statement.
        self._writer.write_line('}')

        # Used by _gen_array_deserializer for arrays of variant.
        return _get_field_member_name(field)

    def _gen_variant_deserializer_from_obj(self, field, field_name, bson_element):
        def on_variant_alternative_match(variant_type):
            value_expr = f'{variant_type.cpp_type}::parse(ctxt, {bson_element})'
            if field.optional:
                cpp_type_info = cpp_types.get_cpp_type(field)
                value_expr = f'{cpp_type_info.get_getter_setter_type()}({value_expr})'
            if field.chained_struct_field:
                chain_source = _get_field_member_name(field.chained_struct_field)
                setter = _get_field_member_setter_name(field)
                self._writer.write_line(f'{chain_source}.{setter}({value_expr});')
            else:
                self._writer.write_line(f'{field_name} = {value_expr};')

        struct_types_list = field.type.variant_struct_types
        if len(struct_types_list) == 1:
            on_variant_alternative_match(struct_types_list[0])
        else:
            key = f'{bson_element}.firstElement().fieldNameStringData()'
            with self._block('[&](StringData s) {', f'}}({key});'):
                with self._block('auto onMatch = [&](int found) {', '};'):
                    with self._block('switch (found) {', '}'):
                        for idx, variant_type in enumerate(struct_types_list):
                            with self._block(f'case {idx}: {{', '} break;'):
                                on_variant_alternative_match(variant_type)
                with self._block('auto onFail = [&] {', '};'):
                    self._writer.write_line('ctxt.throwUnknownField(s);')
                writer.gen_string_table_find_function_block(
                    self._writer, 's', 'onMatch({})', 'onFail()',
                    [f.first_element_field_name for f in struct_types_list])

    def _gen_usage_check(self, field, bson_element, field_usage_check):
        # type: (ast.Field, str, _FieldUsageCheckerBase) -> None
        """Generate the field usage check and insert the required field check."""
        if field_usage_check:
            field_usage_check.add(field, bson_element)

            if _is_required_serializer_field(field):
                self._writer.write_line(_gen_mark_present(field.cpp_name))

    def gen_field_deserializer(self, field, field_type, bson_object, bson_element,
                               field_usage_check, tenant, is_command_field=False, check_type=True):
        # type: (ast.Field, ast.Type, str, str, _FieldUsageCheckerBase, str, bool, bool) -> None
        """Generate the C++ deserializer piece for a field.

        If field_type is scalar and check_type is True (the default), generate type-checking code.
        Array elements are always type-checked.
        """

        # Internal-only types aren't serialized or deserialized.
        if field_type.internal_only:
            return

        if field_type.is_array:
            predicate = "MONGO_likely(ctxt.checkAndAssertType(%s, Array))" % (bson_element)
            with self._predicate(predicate):
                self._gen_usage_check(field, bson_element, field_usage_check)
                self._gen_array_deserializer(field, bson_element, field_type, tenant)
            return

        elif field_type.is_variant:
            self._gen_usage_check(field, bson_element, field_usage_check)
            self._gen_variant_deserializer(field, bson_element, tenant)
            return

        def validate_and_assign_or_uassert(field, expression):
            # type: (ast.Field, str) -> None
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

            if field_type.is_struct:
                # Do not generate a new parser context, reuse the current one since we are not
                # entering a nested document.
                expression = '%s::parse(ctxt, %s)' % (field_type.cpp_type, bson_object)
            else:
                method_name = writer.get_method_name_from_qualified_method_name(
                    field_type.deserializer)
                expression = "%s(%s)" % (method_name, bson_object)

            self._gen_usage_check(field, bson_element, field_usage_check)
            validate_and_assign_or_uassert(field, expression)

        else:
            predicate = None
            if check_type:
                predicate = _get_bson_type_check(bson_element, 'ctxt', field_type)

            with self._predicate(predicate):

                self._gen_usage_check(field, bson_element, field_usage_check)

                object_value = self._gen_field_deserializer_expression(
                    bson_element, field, field_type, tenant)
                if field.chained_struct_field:
                    if field.optional:
                        # We must invoke the boost::optional constructor when setting optional view
                        # types
                        cpp_type_info = cpp_types.get_cpp_type(field)
                        object_value = '%s(%s)' % (cpp_type_info.get_getter_setter_type(),
                                                   object_value)

                    # No need for explicit validation as setter will throw for us.
                    self._writer.write_line(
                        '%s.%s(%s);' % (_get_field_member_name(field.chained_struct_field),
                                        _get_field_member_setter_name(field), object_value))
                else:
                    validate_and_assign_or_uassert(field, object_value)

            if is_command_field and predicate:
                with self._block('else {', '}'):
                    self._writer.write_line(
                        'ctxt.throwMissingField(%s);' % (_get_field_constant_name(field)))

    def gen_doc_sequence_deserializer(self, field, tenant):
        # type: (ast.Field, str) -> None
        """Generate the C++ deserializer piece for a C++ mongo::OpMsg::DocumentSequence."""
        cpp_type_info = cpp_types.get_cpp_type(field)
        cpp_type = cpp_type_info.get_type_name()

        self._writer.write_line('std::vector<%s> values;' % (cpp_type))
        self._writer.write_line('values.reserve(sequence.objs.size());')
        self._writer.write_empty_line()

        # TODO: add support for sequence length checks, today we allow an empty document sequence
        # because we do not give a way for IDL specifications to specify if they allow empty
        # sequences or require non-empty sequences.

        with self._block('for (const BSONObj& sequenceObject : sequence.objs) {', '}'):

            # Either we are deserializing BSON Objects or IDL structs
            if field.type.is_struct:
                validated_tenancy_scope = 'ctxt.getValidatedTenancyScope()'
                if 'request' in tenant:
                    validated_tenancy_scope = 'request.validatedTenancyScope'
                self._writer.write_line('IDLParserContext tempContext(%s, &ctxt, %s, %s, %s);' %
                                        (_get_field_constant_name(field), validated_tenancy_scope,
                                         "getSerializationContext()", tenant))
                array_value = '%s::parse(tempContext, sequenceObject)' % (field.type.cpp_type, )
            elif field.type.is_variant:
                self._writer.write_line('%s _tmp;' % field.type.cpp_type)
                self._gen_variant_deserializer_from_obj(field, field_name='_tmp',
                                                        bson_element='sequenceObject')
                array_value = '_tmp'
            else:
                for serialization_type in field.type.bson_serialization_type:
                    assert serialization_type == 'object'
                if field.type.deserializer:
                    array_value = '%s(sequenceObject)' % (field.type.deserializer)
                else:
                    array_value = "sequenceObject"

            self._writer.write_line('values.emplace_back(%s);' % (array_value))

        self._writer.write_line('%s = std::move(values);' % (_get_field_member_name(field)))

    def gen_op_msg_request_namespace_check(self, struct):
        # type: (ast.Struct) -> None
        """Generate a namespace check for a command."""
        if not isinstance(struct, ast.Command):
            return

        with self._predicate("firstFieldFound == false"):
            # Get the Command element if we need it for later in the deserializer to get the
            # namespace
            if struct.namespace != common.COMMAND_NAMESPACE_IGNORED:
                self._writer.write_line('commandElement = element;')

            self._writer.write_line('firstFieldFound = true;')
            self._writer.write_line('continue;')

    def _gen_initializer_vars(self, constructor, is_command, is_catalog_ctxt):
        # type: (struct_types.MethodInfo, bool, bool) -> List[str]
        """
        Iterate through our list of constructor arguments, which includes serializationContext.

        When we hit serializationContext, build out the conditional statements used to initialize
        them according the structure type and flags; these are similar to the predicates used in
        parseProtected().

        If the structure is not a top-level struct (ie. not a command or is_command_reply resolves
        to false), we want to grab the context from the incoming argument. If we set the structure
        to use a catalog context through the `is_catalog_ctxt` IDL flag, serialze for catalog instead.
        If the incoming argument isn't set we want to use the default constructor. Once we have our 
        expression, we need to put it at the beginning of the list.  This is important because we 
        will be consuming the local copy of the _serializationContext in the same initializer list 
        when we are passing it into the constructor of a nested struct.  In C++, initializer lists 
        are ordered by declaration order, which also identifies the order of initialization.
        """

        initializer_vars = []  # type: List[str]

        for arg in constructor.args:
            # Note that in generate() we push internal_only types to the front when building
            # declarations. However, constructor.args loses that information as the Args() object is
            # constructed via a list of strings, so the order that internal_only types appear in the
            # init list is subject to the order in the hardcoded list passed to the constructor.
            # For now, _serializationContext is the only internal_only type, so additional work
            # around this will be deferred.
            if arg.name == 'serializationContext':
                if is_catalog_ctxt:
                    sc_conditional = 'SerializationContext::stateCatalog()'
                elif is_command:
                    sc_conditional = 'SerializationContext::stateCommandRequest()'
                else:
                    sc_conditional = '_isCommandReply ? SerializationContext::stateCommandReply() : SerializationContext::stateDefault()'

                # this obj is passed in as a boost::optional, so we set the default if no value
                initializer_var = arg.name + '.value_or(%s)' % sc_conditional

                # local _serializationContext obj used to init other structs, so it needs to be
                # initialized first; don't move in the event a boost::none is supplied
                initializer_vars.insert(0, '_%s(%s)' % (arg.name, initializer_var))
            else:
                initializer_vars.append('_%s(std::move(%s))' % (arg.name, arg.name))

        return initializer_vars

    def _gen_constructor(self, struct, constructor, default_init):
        # type: (ast.Struct, struct_types.MethodInfo, bool) -> None
        """Generate the C++ constructor definition."""

        initializers = self._gen_initializer_vars(constructor, isinstance(struct, ast.Command),
                                                  struct.is_catalog_ctxt)

        # Serialize non-has fields first
        # Initialize int and other primitive fields to -1 to prevent Coverity warnings.
        if default_init:
            for field in struct.fields:
                needs_init = (field.type and field.type.cpp_type and not field.type.is_array
                              and _is_required_serializer_field(field)
                              and field.cpp_name != 'dbName')
                if needs_init:
                    # As per _gen_initializer_vars(), we initialize _serializationContext first,
                    # before anything else in the initializer list because it can be consumed by
                    # other member variables in this list.

                    # If the current field is a nested struct, we need to pass the initialized
                    # _serializationContext into the nested struct.
                    serialization_ctx_arg = '_serializationContext' if field.type and field.type.is_struct else ''

                    initializers.append('%s(mongo::idl::preparsedValue<decltype(%s)>(%s))'
                                        % (_get_field_member_name(field),
                                           _get_field_member_name(field), serialization_ctx_arg))

        # Serialize the _dbName field second.
        # Use the class member to perform the initialization instead of the constructor
        # argument, since we're guaranteed to have already written the initializer for the member earlier
        # in the list via _gen_initializer_vars above.
        initializes_db_name = False
        if [arg for arg in constructor.args if arg.name == 'nss']:
            if [field for field in struct.fields if field.serialize_op_msg_request_only]:
                initializers.append('_dbName(_nss.dbName())')
                initializes_db_name = True
        elif [arg for arg in constructor.args if arg.name == 'nssOrUUID']:
            if [field for field in struct.fields if field.serialize_op_msg_request_only]:
                initializers.append('_dbName(_nssOrUUID.dbName())')
                initializes_db_name = True

        initializers_str = ''
        if initializers:
            initializers_str = ': ' + ', '.join(initializers)

        with self._block('%s %s {' % (constructor.get_definition(), initializers_str), '}'):
            for field in _get_required_fields(struct):
                if not (field.name == "$db" and initializes_db_name) and not default_init:
                    self._writer.write_line(_gen_mark_present(field.cpp_name))
            if initializes_db_name:
                self._writer.write_line(_gen_mark_present('dbName'))
        self._writer.write_empty_line()

    def gen_constructors(self, struct):
        # type: (ast.Struct) -> None
        """Generate all the C++ constructor definitions."""

        struct_type_info = struct_types.get_struct_info(struct)
        constructor = struct_type_info.get_constructor_method()

        self._gen_constructor(struct, constructor, True)

        required_constructor = struct_type_info.get_required_constructor_method()
        if len(required_constructor.args) != len(constructor.args):
            # print(struct.name + ": "+  str(required_constructor.args))
            self._gen_constructor(struct, required_constructor, False)

    def gen_field_list_entry_lookup_methods_struct(self, struct):
        # type: (ast.Struct) -> None
        """Generate the definitions for generic argument or reply field lookup methods."""
        field_list_info = generic_field_list_types.get_field_list_info(struct)
        has_field_defn = field_list_info.get_has_field_method().get_definition()
        with self._block(f'{has_field_defn} {{', '}'):
            self._writer.write_line(
                'return FieldInfo::findStructField(fieldName, [](auto) { return true; }, []{ return false; });'
            )

        self._writer.write_empty_line()

        should_fwd_defn = field_list_info.get_should_forward_method().get_definition()
        with self._block(f'{should_fwd_defn} {{', '}'):
            self._writer.write_line(
                'return FieldInfo::findForwardingDisabledField(fieldName, [](auto) { return false; }, []{ return true; });'
            )

        self._writer.write_empty_line()

    def _gen_command_deserializer(self, struct, bson_object, tenant):
        # type: (ast.Struct, str, str) -> None
        """Generate the command field deserializer."""

        if isinstance(struct, ast.Command) and struct.command_field:
            with self._block('{', '}'):
                self.gen_field_deserializer(struct.command_field, struct.command_field.type,
                                            bson_object, "commandElement", None, tenant,
                                            is_command_field=True, check_type=True)
        else:
            struct_type_info = struct_types.get_struct_info(struct)

            # Generate namespace check now that "$db" has been read or defaulted
            struct_type_info.gen_namespace_check(self._writer, "_dbName", "commandElement")

    def _gen_fields_deserializer_common(self, struct, bson_object, tenant):
        # type: (ast.Struct, str, str) -> _FieldUsageCheckerBase
        """Generate the C++ code to deserialize list of fields."""

        field_usage_check = _get_field_usage_checker(self._writer, struct)
        if isinstance(struct, ast.Command):
            self._writer.write_line('BSONElement commandElement;')
            self._writer.write_line('bool firstFieldFound = false;')
            self._writer.write_empty_line()

            # Update the serialization context whether or not we received a tenantId object
            if tenant == 'request.getValidatedTenantId()':
                # inject a context into the IDLParserContext that tags the class as a command request
                self._writer.write_line(
                    'setSerializationContext(SerializationContext::stateCommandRequest());')
                with self._block(
                        'if (request.validatedTenancyScope != boost::none && request.validatedTenancyScope->isFromAtlasProxy()) {',
                        '}'):
                    self._writer.write_line('_serializationContext.setPrefixState(true);')
            else:
                # if a non-default serialization context was passed in via the IDLParserContext,
                # use that to set the local serialization context, otherwise set it to a command
                # request
                with self._block(
                        'if (ctxt.getSerializationContext() != SerializationContext::stateDefault()) {',
                        '}'):
                    self._writer.write_line(
                        'setSerializationContext(ctxt.getSerializationContext());')
                with self._block('else {', '}'):
                    self._writer.write_line(
                        'setSerializationContext(SerializationContext::stateCommandRequest());')

        else:
            if struct.is_catalog_ctxt:
                self._writer.write_line(
                    'setSerializationContext(ctxt.getSerializationContext() == SerializationContext::stateDefault() ? SerializationContext::stateCatalog() : ctxt.getSerializationContext());'
                )
            else:
                # set the local serializer flags according to the constexpr set by is_command_reply
                self._writer.write_line(
                    'setSerializationContext(_isCommandReply ? SerializationContext::stateCommandReply() : ctxt.getSerializationContext());'
                )

        self._writer.write_empty_line()

        field_name_map = {f.name: f for f in struct.fields}

        def map_field(field_name):
            field = field_name_map[field_name]

            if field.ignore:
                field_usage_check.add(field, "element")
                self._writer.write_line('// ignore field')
            else:
                self.gen_field_deserializer(field, field.type, bson_object, "element",
                                            field_usage_check, tenant)
            self._writer.write_line('return true;')

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

            parsed_fields = [f for f in struct.fields if _is_parse(f)]
            if parsed_fields:
                with self._block('auto onMatch = [&](Field f) {', '};'):
                    with self._block('switch (f) {', '}'):
                        for f in parsed_fields:
                            with self._block(f'case {_get_field_enum(f)}: {{', '} break;'):
                                map_field(f.name)
                        self._writer.write_line('default: return false;')
                    self._writer.write_line('return false;')
                with self._block('auto onFail = [] {', '};'):
                    self._writer.write_line('return false;')
                self._writer.write_line(
                    'if (FieldInfo::findParsedField(fieldName, onMatch, onFail)) continue;')

            # End of for fields
            # Generate strict check for extranous fields
            if struct.strict:
                # For commands, check if this is a well known command field that the IDL parser
                # should ignore regardless of strict mode.
                command_predicate = None
                if isinstance(struct, ast.Command):
                    command_predicate = "!mongo::isGenericArgument(fieldName)"

                # Ditto for command replies
                if struct.is_command_reply:
                    command_predicate = "!mongo::isGenericReply(fieldName)"

                with self._predicate(command_predicate):
                    self._writer.write_line('ctxt.throwUnknownField(fieldName);')
            elif not struct.unsafe_dangerous_disable_extra_field_duplicate_checks:
                self._writer.write_line('auto push_result = usedFieldSet.insert(fieldName);')
                with writer.IndentedScopedBlock(
                        self._writer, 'if (MONGO_unlikely(push_result.second == false)) {', '}'):
                    self._writer.write_line('ctxt.throwDuplicateField(fieldName);')

        # Parse chained structs if not inlined
        # Parse chained types always here
        for field in struct.fields:
            if not field.chained or \
                    (field.chained and field.type.is_struct and struct.inline_chained_structs):
                continue

            # Simply generate deserializers since these are all 'any' types
            self.gen_field_deserializer(field, field.type, bson_object, "element", None, tenant)
            self._writer.write_empty_line()

        self._writer.write_empty_line()

        return field_usage_check

    def get_bson_deserializer_static_common(self, struct, static_method_info, method_info,
                                            ownership):
        # type: (ast.Struct, struct_types.MethodInfo, struct_types.MethodInfo, _StructDataOwnership) -> None
        """Generate the C++ deserializer static method."""
        func_def = static_method_info.get_definition()

        with self._block('%s {' % (func_def), '}'):
            if isinstance(struct,
                          ast.Command) and struct.namespace != common.COMMAND_NAMESPACE_IGNORED:
                if struct.namespace == common.COMMAND_NAMESPACE_TYPE:
                    cpp_type_info = cpp_types.get_cpp_type(struct.command_field)

                    if struct.command_field.type.cpp_type and cpp_types.is_primitive_scalar_type(
                            struct.command_field.type.cpp_type):
                        self._writer.write_line(
                            'auto localCmdType = mongo::idl::preparsedValue<%s>();' %
                            (cpp_type_info.get_storage_type()))
                    else:
                        self._writer.write_line(
                            'auto localCmdType = mongo::idl::preparsedValue<%s>();' %
                            (cpp_type_info.get_storage_type()))
                    self._writer.write_line(
                        '%s object(localCmdType);' % (common.title_case(struct.cpp_name)))
                elif struct.namespace in (common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB,
                                          common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB_OR_UUID):
                    self._writer.write_line('NamespaceString localNS;')
                    self._writer.write_line(
                        '%s object(localNS);' % (common.title_case(struct.cpp_name)))
                else:
                    assert False, "Missing case"
            else:
                self._writer.write_line('auto object = mongo::idl::preparsedValue<%s>();' %
                                        common.title_case(struct.cpp_name))

            self._writer.write_line(method_info.get_call('object'))

            if struct.is_view:
                if ownership == _StructDataOwnership.OWNER:
                    self._writer.write_line('object.setAnchor(std::move(bsonObject));')

                elif ownership == _StructDataOwnership.SHARED:
                    self._writer.write_line('object.setAnchor(bsonObject);')

            self._writer.write_line('return object;')

        self.write_empty_line()

    def _compare_and_return_status(self, op, limit, field, optional_param):
        # type: (str, ast.Expression, ast.Field, str) -> None
        """Throw an error on comparison failure."""
        with self._block('if (!(value %s %s)) {' % (op, _get_expression(limit)), '}'):
            self._writer.write_line(
                'throwComparisonError<%s>(%s"%s", "%s"_sd, value, %s);' %
                (field.type.cpp_type, optional_param, field.name, op, _get_expression(limit)))

    def _gen_field_validator(self, struct, field, optional_params):
        # type: (ast.Struct, ast.Field, Tuple[str, str]) -> None
        """Generate non-trivial field validators."""
        validator = field.validator

        cpp_type_info = cpp_types.get_cpp_type_without_optional(field)
        param_type = cpp_type_info.get_storage_type()

        if not cpp_types.is_primitive_type(param_type):
            param_type += '&'

        method_template = {
            'class_name': common.title_case(struct.cpp_name),
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

            for optional_params in [('IDLParserContext& ctxt, ', 'ctxt, '), ('', '')]:
                self._gen_field_validator(struct, field, optional_params)

    def gen_bson_deserializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ deserializer method definitions."""
        struct_type_info = struct_types.get_struct_info(struct)
        method = struct_type_info.get_deserializer_method()

        self.get_bson_deserializer_static_common(struct,
                                                 struct_type_info.get_deserializer_static_method(),
                                                 method, _StructDataOwnership.VIEW)
        self.get_bson_deserializer_static_common(
            struct, struct_type_info.get_sharing_deserializer_static_method(), method,
            _StructDataOwnership.SHARED)
        self.get_bson_deserializer_static_common(
            struct, struct_type_info.get_owned_deserializer_static_method(), method,
            _StructDataOwnership.OWNER)

        func_def = method.get_definition()

        # Name of the variable that we are deserialzing from
        variable_name = "bsonObject"

        with self._block('%s {' % (func_def), '}'):
            # If the struct contains no fields, there's nothing to deserialize, so we write an empty function stub.
            if not struct.fields:
                return
            # if the only field is an internal only field, there's also nothing to deserialize
            if len(struct.fields) == 1 and any(field.type.internal_only for field in struct.fields):
                return

            # Deserialize all the fields
            field_usage_check = self._gen_fields_deserializer_common(struct, variable_name,
                                                                     "ctxt.getTenantId()")

            # Check for required fields
            field_usage_check.add_final_checks()
            self._writer.write_empty_line()

            if struct.cpp_validator_func is not None:
                self._writer.write_line(struct.cpp_validator_func + "(this);")

            self._gen_command_deserializer(struct, variable_name, "ctxt.getTenantId()")

    def gen_op_msg_request_deserializer_methods(self, struct):
        # type: (ast.Struct) -> None
        """Generate the C++ deserializer method definitions from OpMsgRequest."""
        # Commands that have concatentate_with_db namespaces require db name as a parameter
        # 'Empty' structs (those with no fields) don't need to be deserialized
        if not isinstance(struct, ast.Command):
            return

        struct_type_info = struct_types.get_struct_info(struct)

        self.get_bson_deserializer_static_common(
            struct, struct_type_info.get_op_msg_request_deserializer_static_method(),
            struct_type_info.get_op_msg_request_deserializer_method(), _StructDataOwnership.VIEW)

        func_def = struct_type_info.get_op_msg_request_deserializer_method().get_definition()
        with self._block('%s {' % (func_def), '}'):

            # Deserialize all the fields
            field_usage_check = self._gen_fields_deserializer_common(
                struct, "request.body", "request.getValidatedTenantId()")

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
                                self._writer.write_line(_gen_mark_present(field.cpp_name))

                            self.gen_doc_sequence_deserializer(field,
                                                               "request.getValidatedTenantId()")

                        if first_field:
                            first_field = False

                    # End of for fields
                    # Generate strict check for extranous fields
                    with self._block('else {', '}'):
                        if struct.strict:
                            self._writer.write_line('ctxt.throwUnknownField(sequence.name);')
                        else:
                            self._writer.write_line(
                                'auto push_result = usedFieldSet.insert(sequence.name);')
                            with writer.IndentedScopedBlock(
                                    self._writer,
                                    'if (MONGO_unlikely(push_result.second == false)) {', '}'):
                                self._writer.write_line('ctxt.throwDuplicateField(sequence.name);')

                self._writer.write_empty_line()

            # Check for required fields
            field_usage_check.add_final_checks()
            self._writer.write_empty_line()

            self._gen_command_deserializer(struct, "request.body", "request.getValidatedTenantId()")

    def _gen_serializer_method_custom(self, field):
        # type: (ast.Field) -> None
        """Generate the serialize method definition for a custom type."""

        # Generate custom serialization
        template_params = {
            'field_name': _get_field_constant_name(field),
            'access_member': _access_member(field),
            'serialization_context': '_serializationContext',
            'serialization_options': 'options',
        }

        with self._with_template(template_params):
            # Is this a scalar bson C++ type?
            bson_cpp_type = cpp_types.get_bson_cpp_type(field.type)

            # Object types need to go through the generic custom serialization code below
            if bson_cpp_type and bson_cpp_type.has_serializer():
                if field.type.is_array:
                    self._writer.write_template(
                        'BSONArrayBuilder arrayBuilder(builder->subarrayStart(${field_name}));')
                    with self._block('for (const auto& item : ${access_member}) {', '}'):
                        expression = bson_cpp_type.gen_serializer_expression(
                            self._writer, 'item',
                            field.query_shape == ast.QueryShapeFieldType.CUSTOM)
                        template_params['expression'] = expression
                        self._writer.write_template('arrayBuilder.append(${expression});')
                else:
                    expression = bson_cpp_type.gen_serializer_expression(
                        self._writer, _access_member(field),
                        field.query_shape == ast.QueryShapeFieldType.CUSTOM)
                    template_params['expression'] = expression
                    if not field.should_serialize_with_options:
                        self._writer.write_template(
                            'builder->append(${field_name}, ${expression});')
                    elif field.query_shape == ast.QueryShapeFieldType.LITERAL:
                        self._writer.write_template(
                            'options.serializeLiteral(${expression}).serializeForIDL(${field_name}, builder);'
                        )
                    elif field.query_shape == ast.QueryShapeFieldType.ANONYMIZE:
                        self._writer.write_template(
                            'builder->append(${field_name}, options.serializeFieldPathFromString(${expression}));'
                        )
                    else:
                        assert False

            elif field.type.bson_serialization_type[0] == 'any':

                def maybe_add_serialization_context(*args):
                    """Append 'SerializationContext' arg if needed."""
                    if field.type.deserialize_with_tenant:
                        args += ('${serialization_context}', )
                    return args

                def maybe_add_serialization_options(*args):
                    """Append 'SerializationOptions' arg if needed."""
                    if field.query_shape == ast.QueryShapeFieldType.CUSTOM:
                        args += ('${serialization_options}', )
                    return args

                def generate_args_template(*args):
                    """Simply return a string, with all the arguments separated by a comma."""
                    return ", ".join(args)

                def generate_call_site(serializer, subject, *args):
                    """Generate either a function or a method invocation with the appropriate arguments."""
                    args = maybe_add_serialization_context(*args)
                    args = maybe_add_serialization_options(*args)
                    if writer.is_function(serializer):
                        # It should be invoked as 'function(subject, ...args);'
                        return f"{serializer}({generate_args_template(subject, *args)});"
                    else:
                        # It should be invoked as 'subject.method(...args);'
                        truncated_serializer = writer.get_method_name(serializer)
                        return f"{subject}.{truncated_serializer}({generate_args_template(*args)});"

                if field.type.is_array:
                    # Array variants - we pass an array builder
                    self._writer.write_template(
                        'BSONArrayBuilder arrayBuilder(builder->subarrayStart(${field_name}));')
                    with self._block('for (const auto& item : ${access_member}) {', '}'):
                        template = generate_call_site(
                            field.type.serializer,
                            'item',
                            '&arrayBuilder',
                        )
                        self._writer.write_template(template)
                else:
                    # Non-array variants - we pass the field name they should use, and a BSONObjBuilder.
                    template = generate_call_site(field.type.serializer, '${access_member}',
                                                  '${field_name}', 'builder')
                    self._writer.write_template(template)

            else:
                method_name = writer.get_method_name(field.type.serializer)
                template_params['method_name'] = method_name

                if field.chained:
                    # Just directly call the serializer for chained structs without opening up a
                    # nested document.
                    self._writer.write_template('${access_member}.${method_name}(builder);')
                elif field.type.is_array:
                    self._writer.write_template(
                        'BSONArrayBuilder arrayBuilder(builder->subarrayStart(${field_name}));')
                    with self._block('for (const auto& item : ${access_member}) {', '}'):
                        self._writer.write_line(
                            'BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());')
                        self._writer.write_template('item.${method_name}(&subObjBuilder);')
                else:
                    self._writer.write_template(
                        '${access_member}.${method_name}(${field_name}, builder);')

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
                if not field.should_serialize_with_options:
                    self._writer.write_template('${access_member}.serialize(builder);')
                else:
                    self._writer.write_template('${access_member}.serialize(builder, options);')

            elif field.type.is_array:
                self._writer.write_template(
                    'BSONArrayBuilder arrayBuilder(builder->subarrayStart(${field_name}));')
                with self._block('for (const auto& item : ${access_member}) {', '}'):
                    self._writer.write_line(
                        'BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());')
                    if not field.should_serialize_with_options:
                        self._writer.write_line('item.serialize(&subObjBuilder);')
                    else:
                        self._writer.write_line('item.serialize(&subObjBuilder, options);')
            else:
                self._writer.write_template(
                    'BSONObjBuilder subObjBuilder(builder->subobjStart(${field_name}));')
                if not field.should_serialize_with_options:
                    self._writer.write_template('${access_member}.serialize(&subObjBuilder);')
                else:
                    self._writer.write_template(
                        '${access_member}.serialize(&subObjBuilder, options);')

    def _gen_serializer_method_array_variant(self, field):
        template_params = {
            'field_name': _get_field_constant_name(field),
            'access_member': 'item',
        }

        with self._with_template(template_params):
            self._writer.write_template(
                'BSONArrayBuilder arrayBuilder(builder->subarrayStart(${field_name}));')
            with self._block('for (const auto& item : %s) {' % _access_member(field), '}'):
                self._writer.write_line('BSONObjBuilder subObjBuilder(arrayBuilder.subobjStart());')
                self._gen_serializer_method_variant_helper(field, template_params,
                                                           builder='&subObjBuilder')

    def _gen_serializer_method_variant(self, field):
        # type: (ast.Field) -> None
        """Generate the serialize method definition for a variant type."""
        template_params = {
            'field_name': _get_field_constant_name(field),
            'access_member': _access_member(field),
        }

        with self._with_template(template_params):
            self._gen_serializer_method_variant_helper(field, template_params)

    def _gen_serializer_method_variant_helper(self, field, template_params, builder='builder'):
        # type: (ast.Field, Dict[str, str], str) -> None

        with self._block('visit(OverloadedVisitor{', '}, ${access_member});'):
            for variant_type in itertools.chain(
                    field.type.variant_types,
                    field.type.variant_struct_types if field.type.variant_struct_types else []):

                template_params[
                    'cpp_type'] = 'std::vector<' + variant_type.cpp_type + '>' if variant_type.is_array else variant_type.cpp_type

                template_params['param_opt'] = ""
                if field.should_serialize_with_options:
                    template_params['param_opt'] = ', options'
                with self._block('[%s${param_opt}](const ${cpp_type}& value) {' % builder, '},'):
                    bson_cpp_type = cpp_types.get_bson_cpp_type(variant_type)
                    if field.type.is_variant and field.type.is_array:
                        self._writer.write_template('value.serialize(%s);' % builder)
                    elif bson_cpp_type and bson_cpp_type.has_serializer():
                        assert not field.type.is_array
                        expression = bson_cpp_type.gen_serializer_expression(
                            self._writer, 'value',
                            field.query_shape == ast.QueryShapeFieldType.CUSTOM)
                        template_params['expression'] = expression
                        if not field.should_serialize_with_options:
                            self._writer.write_template(
                                'builder->append(${field_name}, ${expression});')
                        elif field.query_shape == ast.QueryShapeFieldType.LITERAL:
                            self._writer.write_template(
                                'options.serializeLiteral(${expression}).serializeForIDL(${field_name}, builder);'
                            )
                        elif field.query_shape == ast.QueryShapeFieldType.ANONYMIZE:
                            self._writer.write_template(
                                'builder->append(${field_name}, options.serializeFieldPathFromString(${expression}));'
                            )
                        else:
                            assert False
                    else:
                        if not field.should_serialize_with_options:
                            self._writer.write_template(
                                'idl::idlSerialize(builder, ${field_name}, value);')
                        elif field.query_shape == ast.QueryShapeFieldType.LITERAL:
                            self._writer.write_template(
                                'options.serializeLiteral(value).serializeForIDL(${field_name}, builder);'
                            )
                        elif field.query_shape == ast.QueryShapeFieldType.ANONYMIZE:
                            self._writer.write_template(
                                'idl::idlSerialize(builder, ${field_name}, options.serializeFieldPathFromString(value));'
                            )
                        else:
                            assert False

    def _gen_serializer_method_common(self, field):
        # type: (ast.Field) -> None
        """Generate the serialize method definition."""

        member_name = _get_field_member_name(field)

        # Is this a scalar bson C++ type?
        bson_cpp_type = cpp_types.get_bson_cpp_type(field.type)

        needs_custom_serializer = field.type.serializer or (bson_cpp_type
                                                            and bson_cpp_type.has_serializer())

        optional_block_start = None
        if field.optional:
            optional_block_start = 'if (%s) {' % (member_name)
        elif field.type.is_struct or needs_custom_serializer or field.type.is_array:
            # Introduce a new scope for required nested object serialization.
            optional_block_start = '{'

        with self._block(optional_block_start, '}'):
            if not field.type.is_struct:
                if needs_custom_serializer:
                    self._gen_serializer_method_custom(field)
                elif field.type.is_variant:
                    if field.type.is_array:
                        # For array of variants, is_variant == is_array == True.
                        self._gen_serializer_method_array_variant(field)
                    else:
                        self._gen_serializer_method_variant(field)
                else:
                    # Generate default serialization
                    # Note: BSONObjBuilder::append, which all three branches use, has overrides for std::vector also
                    if not field.should_serialize_with_options:
                        self._writer.write_line(
                            'builder->append(%s, %s);' % (_get_field_constant_name(field),
                                                          _access_member(field)))
                    elif field.query_shape == ast.QueryShapeFieldType.LITERAL:
                        # serializeLiteral expects an ImplicitValue, which can't be constructed with an int64_t
                        expression_cast = ""
                        if field.type.cpp_type == "std::int64_t":
                            expression_cast = "(long long)"
                        self._writer.write_line(
                            'options.serializeLiteral(%s%s).serializeForIDL(%s, builder);'
                            % (expression_cast, _access_member(field),
                               _get_field_constant_name(field)))
                    elif field.query_shape == ast.QueryShapeFieldType.ANONYMIZE:
                        self._writer.write_line(
                            'builder->append(%s, options.serializeFieldPathFromString(%s));' %
                            (_get_field_constant_name(field), _access_member(field)))
                    else:
                        assert False
            else:
                self._gen_serializer_method_struct(field)

        if field.always_serialize:
            # If using field.always_serialize, field.optional must also be true. Add an else block
            # that appends null when the optional field is not initialized.
            with self._block('else {', '}'):
                self._writer.write_line(
                    'builder->appendNull(%s);' % (_get_field_constant_name(field)))

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
            self._writer.write_line('_hasMembers.required();')
            self._writer.write_empty_line()

        # Serialize the namespace as the first field
        if isinstance(struct, ast.Command):
            if struct.command_field:
                # Internal-only types aren't serialized or deserialized.
                if not (struct.command_field.type and struct.command_field.type.internal_only):
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

            # Internal-only types aren't serialized or deserialized.
            if field.type and field.type.internal_only:
                continue

            self._gen_serializer_method_common(field)

            # Add a blank line after each block
            self._writer.write_empty_line()

        # Append passthrough elements
        if isinstance(struct, ast.Command):
            known_name = "_knownOP_MSGFields" if is_op_msg_request else "_knownBSONFields"
            self._writer.write_line(
                "IDLParserContext::appendGenericCommandArguments(commandPassthroughFields, %s, builder);"
                % (known_name))
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
                optional_block_start = 'if (%s) {' % (member_name)

            with self._block(optional_block_start, '}'):
                self._writer.write_line('OpMsg::DocumentSequence documentSequence;')
                self._writer.write_template(
                    'documentSequence.name = %s.toString();' % (_get_field_constant_name(field)))

                with self._block('for (const auto& item : %s) {' % (_access_member(field)), '}'):
                    if not field.type.is_struct:
                        if field.type.is_variant:
                            # _gen_serializer_method_variant expects builder to be a pointer.
                            self._writer.write_line('BSONObjBuilder objBuilder;')
                            self._writer.write_line('BSONObjBuilder* builder = &objBuilder;')

                            template_params = {
                                'field_name': _get_field_constant_name(field),
                                'access_member': 'item',
                            }

                            with self._with_template(template_params):
                                self._gen_serializer_method_variant_helper(field, template_params)

                            self._writer.write_line(
                                'documentSequence.objs.push_back(builder->obj());')
                        elif field.type.serializer:
                            self._writer.write_line('documentSequence.objs.push_back(item.%s());' %
                                                    (writer.get_method_name(field.type.serializer)))
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
        if not isinstance(struct, ast.Command):
            return

        struct_type_info = struct_types.get_struct_info(struct)

        with self._block(
                '%s {' % (struct_type_info.get_op_msg_request_serializer_method().get_definition()),
                '}'):
            self._writer.write_line('BSONObjBuilder localBuilder;')

            with self._block('{', '}'):
                self._writer.write_line('BSONObjBuilder* builder = &localBuilder;')

                self._gen_serializer_methods_common(struct, True)

            self._writer.write_line('OpMsgRequest request;')
            self._writer.write_line('request.body = localBuilder.obj();')

            self._gen_doc_sequence_serializer(struct)

            self._writer.write_line('return request;')

    def gen_authorization_contract_definition(self, struct):
        # type: (ast.Struct) -> None
        """Generate the authorization contract defintion."""

        if not isinstance(struct, ast.Command):
            return

        # None means access_checks was not specified, empty list means it has "none: true"
        if struct.access_checks is None:
            return

        checks_list = [ac.check for ac in struct.access_checks if ac.check]
        checks = ",".join([("AccessCheckEnum::" + c) for c in checks_list])

        privilege_list = [ac.privilege for ac in struct.access_checks if ac.privilege]
        privileges = ",".join([
            "Privilege(ResourcePattern::forAuthorizationContract(MatchTypeEnum::%s), ActionSet{%s})"
            % (p.resource_pattern, ",".join(["ActionType::" + at for at in p.action_type]))
            for p in privilege_list
        ])

        self._writer.write_line(
            'mongo::AuthorizationContract %s::kAuthorizationContract = AuthorizationContract(std::initializer_list<AccessCheckEnum>{%s}, std::initializer_list<Privilege>{%s});'
            % (common.title_case(struct.cpp_name), checks, privileges))

        self._writer.write_empty_line()

    def gen_enum_definition(self, idl_enum):
        # type: (ast.Enum) -> None
        """Generate the definitions for an enum's supporting functions."""
        enum_type_info = enum_types.get_type_info(idl_enum)

        enum_type_info.gen_deserializer_definition(self._writer)
        self._writer.write_empty_line()

        enum_type_info.gen_serializer_definition(self._writer)
        self._writer.write_empty_line()

        enum_type_info.gen_extra_data_definition(self._writer)

    def _gen_known_fields_declaration(self, struct, name, include_op_msg_implicit):
        # type: (ast.Struct, str, bool) -> None
        """Generate the known fields declaration with specified name."""
        block_name = common.template_args(
            'const std::vector<StringData> ${class_name}::_${name}Fields {', name=name,
            class_name=common.title_case(struct.cpp_name))
        with self._block(block_name, "};"):
            sorted_fields = sorted([
                field for field in struct.fields
                if (not field.serialize_op_msg_request_only or include_op_msg_implicit)
            ], key=lambda f: f.cpp_name)

            for field in sorted_fields:
                # Internal only fields are not parsed from BSON objects
                if field.type and field.type.internal_only:
                    continue

                self._writer.write_line(
                    common.template_args('${class_name}::${constant_name},',
                                         class_name=common.title_case(struct.cpp_name),
                                         constant_name=_get_field_constant_name(field)))

            self._writer.write_line(
                common.template_args('${class_name}::kCommandName,',
                                     class_name=common.title_case(struct.cpp_name)))

    def gen_known_fields_declaration(self, struct):
        # type: (ast.Struct) -> None
        """Generate the all the known fields declarations."""
        if not isinstance(struct, ast.Command):
            return

        self._gen_known_fields_declaration(struct, "knownBSON", False)
        self._gen_known_fields_declaration(struct, "knownOP_MSG", True)

    def gen_field_list_entries_declaration_struct(self, struct):
        # type: (ast.Struct) -> None
        field_list_info = generic_field_list_types.get_field_list_info(struct)
        klass = common.title_case(struct.cpp_name)
        self._writer.write_line(
            common.template_args('// Map: fieldName -> ${should_forward_name}',
                                 should_forward_name=field_list_info.get_should_forward_name()))
        block_name = common.template_args(
            'const StaticImmortal<StringMap<bool>> ${klass}::_genericFields {{', klass=klass)
        with self._block(block_name, "}};"):
            sorted_entries = sorted(struct.fields, key=lambda f: f.name)
            for entry in sorted_entries:
                self._writer.write_line(
                    common.template_args(
                        '{"${name}", ${should_forward}},', klass=klass, name=entry.name,
                        should_forward='true'
                        if entry.generic_field_info.get_should_forward() else 'false'))

    def _gen_server_parameter_specialized(self, param):
        # type: (ast.ServerParameter) -> None
        """Generate a specialized ServerParameter."""
        self._writer.write_line('auto sp = makeServerParameter<%s>(%s, %s);' %
                                (param.cpp_class.name, _encaps(param.name), param.set_at))
        if param.redact:
            self._writer.write_line('sp->setRedact();')
        self._writer.write_line('return sp;')

    def _gen_server_parameter_class_definitions(self, param):
        # type: (ast.ServerParameter) -> None
        """Generate storage for default and/or append method for a specialized ServerParameter."""
        cls = param.cpp_class
        is_cluster_param = (param.set_at == 'ServerParameterType::kClusterWide')

        if param.default or param.redact or is_cluster_param:
            self.gen_description_comment("%s: %s" % (param.name, param.description))

        if param.default:
            self._writer.write_line(
                'constexpr decltype(%s::kDataDefault) %s::kDataDefault;' % (cls.name, cls.name))
            self.write_empty_line()

        if param.redact:
            with self._block(
                    'void %s::append(OperationContext*, BSONObjBuilder* b, StringData name, const boost::optional<TenantId>& tenantId) {'
                    % (cls.name), '}'):
                self._writer.write_line('*b << name << "###";')
            self.write_empty_line()

        # Specialized cluster parameters should also provide the implementation of setFromString().
        if is_cluster_param:
            with self._block(
                    'Status %s::setFromString(StringData str, const boost::optional<TenantId>& tenantId) {'
                    % (cls.name), '}'):
                self._writer.write_line(
                    'return {ErrorCodes::BadValue, "setFromString should never be used with cluster server parameters"};'
                )
            self.write_empty_line()

    def _gen_server_parameter_with_storage(self, param):
        # type: (ast.ServerParameter) -> None
        """Generate a single IDLServerParameterWithStorage."""
        if param.feature_flag:
            self._writer.write_line(
                common.template_args(
                    'auto* ret = makeFeatureFlagServerParameter(${name}, ${storage});',
                    storage=param.cpp_varname, name=_encaps(param.name)))
        else:
            self._writer.write_line(
                common.template_args(
                    'auto* ret = makeIDLServerParameterWithStorage<${spt}>(${name}, ${storage});',
                    storage=param.cpp_varname, spt=param.set_at, name=_encaps(param.name)))

        if param.on_update is not None:
            self._writer.write_line('ret->setOnUpdate(%s);' % (param.on_update))
        if param.validator is not None:
            if param.validator.callback is not None:
                self._writer.write_line('ret->addValidator(%s);' % (param.validator.callback))

            for pred in ['lt', 'gt', 'lte', 'gte']:
                bound = getattr(param.validator, pred)
                if bound is not None:
                    self._writer.write_line('ret->addBound<idl_server_parameter_detail::%s>(%s);' %
                                            (pred.upper(), _get_expression(bound)))

        if param.redact:
            self._writer.write_line('ret->setRedact();')

        if param.default and not (param.cpp_vartype and param.cpp_varname):
            # Only need to call setDefault() if we haven't in-place initialized the declared var.
            self._writer.write_line(
                'uassertStatusOK(ret->setDefault(%s));' % (_get_expression(param.default)))

        self._writer.write_line('return ret;')

    def _gen_server_parameter(self, param):
        # type: (ast.ServerParameter) -> None
        """Generate a single IDLServerParameter(WithStorage)."""
        if param.cpp_class is not None:
            self._gen_server_parameter_specialized(param)
        else:
            self._gen_server_parameter_with_storage(param)

    def _gen_server_parameter_deprecated_aliases(self, param_no, param):
        # type: (int, ast.ServerParameter) -> None
        """Generate IDLServerParamterDeprecatedAlias instance."""

        for alias_no, alias in enumerate(param.deprecated_name):
            self._writer.write_line(
                common.template_args(
                    '${unused} auto* ${alias_var} = makeIDLServerParameterDeprecatedAlias(${name}, ${param_var});',
                    unused='[[maybe_unused]]', alias_var='scp_%d_%d' % (param_no, alias_no),
                    name=_encaps(alias), param_var='scp_%d' % (param_no)))

    def gen_server_parameters(self, params, header_file_name):
        # type: (List[ast.ServerParameter], str) -> None
        """Generate IDLServerParameter instances."""

        for param in params:
            # Definitions for specialized server parameters.
            if param.cpp_class:
                self._gen_server_parameter_class_definitions(param)

            # Optional storage declarations.
            elif (param.cpp_vartype is not None) and (param.cpp_varname is not None):
                with self._condition(param.condition, preprocessor_only=True):
                    init = ('{%s}' % (param.default.expr)) if param.default else ''
                    self._writer.write_line(
                        '%s %s%s;' % (param.cpp_vartype, param.cpp_varname, init))

        blockname = 'idl_' + \
            hashlib.sha1(header_file_name.encode()).hexdigest()
        with self._block('MONGO_SERVER_PARAMETER_REGISTER(%s)(InitializerContext*) {' % (blockname),
                         '}'):
            # ServerParameter instances.
            for param_no, param in enumerate(params):
                self.gen_description_comment(param.description)
                with self._condition(param.condition):
                    unused = not (param.test_only or param.deprecated_name)
                    with self.get_initializer_lambda('auto* scp_%d' % (param_no), unused=unused,
                                                     return_type='ServerParameter*'):
                        self._gen_server_parameter(param)

                    if param.test_only:
                        self._writer.write_line('scp_%d->setTestOnly();' % (param_no))

                    if param.condition:
                        if param.condition.feature_flag:
                            self._writer.write_line('scp_%d->setFeatureFlag(&%s);' %
                                                    (param_no, param.condition.feature_flag))
                        if param.condition.min_fcv:
                            self._writer.write_line(
                                'scp_%d->setMinFCV(FeatureCompatibilityVersionParser::parseVersionForFeatureFlags("%s"));'
                                % (param_no, param.condition.min_fcv))

                    self._gen_server_parameter_deprecated_aliases(param_no, param)
                self.write_empty_line()

    def gen_config_option(self, opt, section):
        # type: (ast.ConfigOption, str) -> None
        """Generate Config Option instance."""

        # Derive cpp_vartype from arg_vartype if needed.
        vartype = ("moe::OptionTypeMap<moe::%s>::type" %
                   (opt.arg_vartype)) if opt.cpp_vartype is None else opt.cpp_vartype

        # Mark option as coming from IDL autogenerated code.
        usage = 'moe::OptionSection::OptionParserUsageType::IDLAutoGeneratedCode'

        with self._condition(opt.condition):
            with self._block(section, ';'):
                self._writer.write_line(
                    common.template_format(
                        '.addOptionChaining(${name}, ${short}, moe::${argtype}, ${desc}, ${deprname}, ${deprshortname}, ${usage})',
                        {
                            'name': _encaps(opt.name),
                            'short': _encaps(opt.short_name),
                            'argtype': opt.arg_vartype,
                            'desc': _get_expression(opt.description),
                            'deprname': _encaps_list(opt.deprecated_name),
                            'deprshortname': _encaps_list(opt.deprecated_short_name),
                            'usage': usage,
                        }))
                self._writer.write_line('.setSources(moe::%s)' % (opt.source))
                if opt.hidden:
                    self._writer.write_line('.hidden()')
                if opt.redact:
                    self._writer.write_line('.redact()')
                for requires in opt.requires:
                    self._writer.write_line('.requiresOption(%s)' % (_encaps(requires)))
                for conflicts in opt.conflicts:
                    self._writer.write_line('.incompatibleWith(%s)' % (_encaps(conflicts)))
                if opt.default:
                    self._writer.write_line(
                        '.setDefault(moe::Value(%s))' % (_get_expression(opt.default)))
                if opt.implicit:
                    self._writer.write_line(
                        '.setImplicit(moe::Value(%s))' % (_get_expression(opt.implicit)))
                if opt.duplicates_append:
                    self._writer.write_line('.composing()')
                if (opt.positional_start is not None) and (opt.positional_end is not None):
                    self._writer.write_line(
                        '.positional(%d, %d)' % (opt.positional_start, opt.positional_end))
                if opt.canonicalize:
                    self._writer.write_line('.canonicalize(%s)' % opt.canonicalize)

                if opt.validator:
                    if opt.validator.callback:
                        self._writer.write_line(
                            common.template_args(
                                '.addConstraint(new moe::CallbackKeyConstraint<${argtype}>(${key}, ${callback}))',
                                argtype=vartype, key=_encaps(opt.name),
                                callback=opt.validator.callback))

                    if (opt.validator.gt is not None) or (opt.validator.lt is not None) or (
                            opt.validator.gte is not None) or (opt.validator.lte is not None):
                        self._writer.write_line(
                            common.template_args(
                                '.addConstraint(new moe::BoundaryKeyConstraint<${argtype}>(${key}, ${gt}, ${lt}, ${gte}, ${lte}))',
                                argtype=vartype, key=_encaps(opt.name), gt='boost::none'
                                if opt.validator.gt is None else _get_expression(opt.validator.gt),
                                lt='boost::none'
                                if opt.validator.lt is None else _get_expression(opt.validator.lt),
                                gte='boost::none' if opt.validator.gte is None else _get_expression(
                                    opt.validator.gte), lte='boost::none' if
                                opt.validator.lte is None else _get_expression(opt.validator.lte)))

        self.write_empty_line()

    def _gen_config_options_register(self, root_opts, sections, returns_status):
        self._writer.write_line('namespace moe = ::mongo::optionenvironment;')
        self.write_empty_line()

        for opt in root_opts:
            self.gen_config_option(opt, 'options')

        for section_name, section_opts in sections.items():
            with self._block('{', '}'):
                self._writer.write_line('moe::OptionSection section(%s);' % (_encaps(section_name)))
                self.write_empty_line()

                for opt in section_opts:
                    self.gen_config_option(opt, 'section')

                self._writer.write_line('auto status = options.addSection(section);')

                if returns_status:
                    with self._block('if (!status.isOK()) {', '}'):
                        self._writer.write_line('return status;')
                else:
                    self._writer.write_line('uassertStatusOK(status);')

            self.write_empty_line()
        if returns_status:
            self._writer.write_line('return Status::OK();')

    def _gen_config_options_store(self, configs, return_status):
        # Setup initializer for storing configured options in their variables.
        self._writer.write_line('namespace moe = ::mongo::optionenvironment;')
        self.write_empty_line()

        for opt in configs:
            if opt.cpp_varname is None:
                continue

            vartype = ("moe::OptionTypeMap<moe::%s>::type" %
                       (opt.arg_vartype)) if opt.cpp_vartype is None else opt.cpp_vartype
            with self._condition(opt.condition):
                with self._block('if (params.count(%s)) {' % (_encaps(opt.name)), '}'):
                    self._writer.write_line(
                        '%s = params[%s].as<%s>();' % (opt.cpp_varname, _encaps(opt.name), vartype))
            self.write_empty_line()

        if return_status:
            self._writer.write_line('return Status::OK();')

    def gen_config_options(self, spec, header_file_name):
        # type: (ast.IDLAST, str) -> None
        """Generate Config Option instances."""

        has_storage_targets = False
        for opt in spec.configs:
            if opt.cpp_varname is not None:
                has_storage_targets = True
                if opt.cpp_vartype is not None:
                    with self._condition(opt.condition, preprocessor_only=True):
                        init = ('{%s}' % (opt.default.expr)) if opt.default else ''
                        self._writer.write_line(
                            '%s %s%s;' % (opt.cpp_vartype, opt.cpp_varname, init))

        self.write_empty_line()

        root_opts = []  # type: List[ast.ConfigOption]
        sections = {}  # type: Dict[str, List[ast.ConfigOption]]
        for opt in spec.configs:
            if opt.section:
                try:
                    sections[opt.section].append(opt)
                except KeyError:
                    sections[opt.section] = [opt]
            else:
                root_opts.append(opt)

        initializer = spec.globals.configs and spec.globals.configs.initializer

        blockname = (initializer and initializer.name) or (
            'idl_' + hashlib.sha1(header_file_name.encode()).hexdigest())

        if initializer and initializer.register:
            with self._block(
                    'Status %s(optionenvironment::OptionSection* options_ptr) {' %
                    initializer.register, '}'):
                self._writer.write_line('auto& options = *options_ptr;')
                self._gen_config_options_register(root_opts, sections, True)
        else:
            with self.gen_namespace_block(''):
                with self._block(
                        'MONGO_MODULE_STARTUP_OPTIONS_REGISTER(%s)(InitializerContext*) {' %
                    (blockname), '}'):
                    self._writer.write_line('auto& options = optionenvironment::startupOptions;')
                    self._gen_config_options_register(root_opts, sections, False)

        self.write_empty_line()

        if has_storage_targets:
            if initializer and initializer.store:
                with self._block(
                        'Status %s(const optionenvironment::Environment& params) {' %
                        initializer.store, '}'):
                    self._gen_config_options_store(spec.configs, True)
            else:
                with self.gen_namespace_block(''):
                    with self._block(
                            'MONGO_STARTUP_OPTIONS_STORE(%s)(InitializerContext*) {' % (blockname),
                            '}'):
                        # If all options are guarded by non-passing #ifdefs, then params will be unused.
                        self._writer.write_line(
                            '[[maybe_unused]] const auto& params = optionenvironment::startupOptionsParsed;'
                        )
                        self._gen_config_options_store(spec.configs, False)

            self.write_empty_line()

    def generate(self, spec, header_file_name):
        # type: (ast.IDLAST, str) -> None
        """Generate the C++ source to a stream."""

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
            'mongo/util/overloaded_visitor.h',
            'mongo/util/string_map.h',
        ]

        if spec.commands:
            header_list.append('mongo/db/auth/authorization_contract.h')
            header_list.append('mongo/idl/command_generic_argument.h')
        elif len([s for s in spec.structs if s.is_command_reply]) > 0:
            header_list.append('mongo/idl/command_generic_argument.h')

        if spec.server_parameters:
            header_list.append('mongo/db/server_parameter.h')
            header_list.append('mongo/db/server_parameter_with_storage.h')

        if spec.configs:
            header_list.append('mongo/util/options_parser/option_section.h')
            header_list.append('mongo/util/options_parser/startup_option_init.h')
            header_list.append('mongo/util/options_parser/startup_options.h')

        header_list.sort()

        for include in header_list:
            self.gen_include(include)

        self.write_empty_line()

        # Generate namespace
        with self.gen_namespace_block(spec.globals.cpp_namespace):
            self.write_empty_line()

            for idl_enum in spec.enums:
                self.gen_description_comment(idl_enum.description)
                self.gen_enum_definition(idl_enum)

            all_structs = spec.structs + cast(List[ast.Struct], spec.commands)

            for struct in all_structs:
                self.gen_field_info(struct)
                self.write_empty_line()

                self.gen_authorization_contract_definition(struct)

                # Write known fields declaration for command
                self.gen_known_fields_declaration(struct)
                self.write_empty_line()

                # Write constructor
                self.gen_constructors(struct)
                self.write_empty_line()

                # Write field validators
                self.gen_field_validators(struct)
                self.write_empty_line()

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

                if struct.generic_list_type:
                    # Write field lookup methods
                    self.gen_field_list_entry_lookup_methods_struct(struct)
                    self.write_empty_line()

            if spec.server_parameters:
                self.gen_server_parameters(spec.server_parameters, header_file_name)
            if spec.configs:
                self.gen_config_options(spec, header_file_name)


def generate_header_str(spec):
    # type: (ast.IDLAST) -> str
    """Generate a C++ header in-memory."""
    stream = io.StringIO()
    text_writer = writer.IndentedTextWriter(stream)

    header = _CppHeaderFileWriter(text_writer)

    header.generate(spec)

    return stream.getvalue()


def _generate_header(spec, file_name):
    # type: (ast.IDLAST, str) -> None
    """Generate a C++ header."""

    str_value = generate_header_str(spec)

    # Generate structs
    with io.open(file_name, mode='wb') as file_handle:
        file_handle.write(str_value.encode())


def generate_source_str(spec, target_arch, header_file_name):
    # type: (ast.IDLAST, str, str) -> str
    """Generate a C++ source file in-memory."""
    stream = io.StringIO()
    text_writer = writer.IndentedTextWriter(stream)

    source = _CppSourceFileWriter(text_writer, target_arch)

    source.generate(spec, header_file_name)

    return stream.getvalue()


def _generate_source(spec, target_arch, file_name, header_file_name):
    # type: (ast.IDLAST, str, str, str) -> None
    """Generate a C++ source file."""
    str_value = generate_source_str(spec, target_arch, header_file_name)

    # Generate structs
    with io.open(file_name, mode='wb') as file_handle:
        file_handle.write(str_value.encode())


def generate_code(spec, target_arch, output_base_dir, header_file_name, source_file_name):
    # type: (ast.IDLAST, str, str, str, str) -> None
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
