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
"""Transform idl.syntax trees from the parser into well-defined idl.ast trees."""

from __future__ import absolute_import, print_function, unicode_literals

import re
# from typing import Union

from . import ast
from . import bson
from . import errors
from . import syntax


def _validate_single_bson_type(ctxt, idl_type, syntax_type):
    # type: (errors.ParserContext, Union[syntax.Type, ast.Field], unicode) -> bool
    """Validate bson serialization type is correct for a type."""
    bson_type = idl_type.bson_serialization_type[0]

    # Any is only valid if it is the only bson type specified
    if bson_type == "any":
        return True

    if not bson.is_valid_bson_type(bson_type):
        ctxt.add_bad_bson_type_error(idl_type, syntax_type, idl_type.name, bson_type)
        return False

    # Validate bindata_subytpe
    if bson_type == "bindata":
        subtype = idl_type.bindata_subtype

        if subtype is None:
            subtype = "<unknown>"

        if not bson.is_valid_bindata_subtype(subtype):
            ctxt.add_bad_bson_bindata_subtype_value_error(idl_type, syntax_type, idl_type.name,
                                                          subtype)
    elif idl_type.bindata_subtype is not None:
        ctxt.add_bad_bson_bindata_subtype_error(idl_type, syntax_type, idl_type.name, bson_type)

    return True


def _validate_bson_types_list(ctxt, idl_type, syntax_type):
    # type: (errors.ParserContext, Union[syntax.Type, ast.Field], unicode) -> bool
    """Validate bson serialization type(s) is correct for a type."""

    bson_types = idl_type.bson_serialization_type
    if len(bson_types) == 1:
        return _validate_single_bson_type(ctxt, idl_type, syntax_type)

    for bson_type in bson_types:
        if bson_type == "any":
            ctxt.add_bad_any_type_use_error(idl_type, syntax_type, idl_type.name)
            return False

        if not bson.is_valid_bson_type(bson_type):
            ctxt.add_bad_bson_type_error(idl_type, syntax_type, idl_type.name, bson_type)
            return False

        # V1 restiction: cannot mix bindata into list of types
        if bson_type == "bindata":
            ctxt.add_bad_bson_type_error(idl_type, syntax_type, idl_type.name, bson_type)
            return False

        # Cannot mix non-scalar types into the list of types
        if not bson.is_scalar_bson_type(bson_type):
            ctxt.add_bad_bson_scalar_type_error(idl_type, syntax_type, idl_type.name, bson_type)
            return False

    return True


def _validate_type(ctxt, idl_type):
    # type: (errors.ParserContext, syntax.Type) -> None
    """Validate each type is correct."""

    # Validate naming restrictions
    if idl_type.name.startswith("array"):
        ctxt.add_array_not_valid_error(idl_type, "type", idl_type.name)

    _validate_type_properties(ctxt, idl_type, 'type')


def _validate_cpp_type(ctxt, idl_type, syntax_type):
    # type: (errors.ParserContext, Union[syntax.Type, ast.Field], unicode) -> None
    """Validate the cpp_type is correct."""

    # Validate cpp_type
    # Do not allow StringData, use std::string instead.
    if "StringData" in idl_type.cpp_type:
        ctxt.add_no_string_data_error(idl_type, syntax_type, idl_type.name)

    # We do not support C++ char and float types for style reasons
    if idl_type.cpp_type in ['char', 'wchar_t', 'char16_t', 'char32_t', 'float']:
        ctxt.add_bad_cpp_numeric_type_use_error(idl_type, syntax_type, idl_type.name,
                                                idl_type.cpp_type)

    # We do not support C++ builtin integer for style reasons
    for numeric_word in ['signed', "unsigned", "int", "long", "short"]:
        if re.search(r'\b%s\b' % (numeric_word), idl_type.cpp_type):
            ctxt.add_bad_cpp_numeric_type_use_error(idl_type, syntax_type, idl_type.name,
                                                    idl_type.cpp_type)
            # Return early so we only throw one error for types like "signed short int"
            return

    # Check for std fixed integer types which are allowed
    if idl_type.cpp_type in ["std::int32_t", "std::int64_t", "std::uint32_t", "std::uint64_t"]:
        return

    # Check for std fixed integer types which are not allowed. These are not allowed even if they
    # have the "std::" prefix.
    for std_numeric_type in [
            "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t"
    ]:
        if std_numeric_type in idl_type.cpp_type:
            ctxt.add_bad_cpp_numeric_type_use_error(idl_type, syntax_type, idl_type.name,
                                                    idl_type.cpp_type)
            return


def _validate_type_properties(ctxt, idl_type, syntax_type):
    # type: (errors.ParserContext, Union[syntax.Type, ast.Field], unicode) -> None
    """Validate each type or field is correct."""

    # Validate bson type restrictions
    if not _validate_bson_types_list(ctxt, idl_type, syntax_type):
        return

    if len(idl_type.bson_serialization_type) == 1:
        bson_type = idl_type.bson_serialization_type[0]
        if bson_type == "any":
            # For any, a deserialer is required but the user can try to get away with the default
            # serialization for their C++ type.
            if idl_type.deserializer is None:
                ctxt.add_missing_ast_required_field_error(idl_type, syntax_type, idl_type.name,
                                                          "deserializer")
        elif bson_type == "object":
            if idl_type.deserializer is None:
                ctxt.add_missing_ast_required_field_error(idl_type, syntax_type, idl_type.name,
                                                          "deserializer")

            if idl_type.serializer is None:
                ctxt.add_missing_ast_required_field_error(idl_type, syntax_type, idl_type.name,
                                                          "serializer")
        elif not bson_type == "string":
            if idl_type.deserializer is not None and "BSONElement" not in idl_type.deserializer:
                ctxt.add_not_custom_scalar_serialization_not_supported_error(
                    idl_type, syntax_type, idl_type.name, bson_type)

            if idl_type.serializer is not None:
                ctxt.add_not_custom_scalar_serialization_not_supported_error(
                    idl_type, syntax_type, idl_type.name, bson_type)
    else:
        # Now, this is a list of scalar types
        if idl_type.deserializer is None:
            ctxt.add_missing_ast_required_field_error(idl_type, syntax_type, idl_type.name,
                                                      "deserializer")

    _validate_cpp_type(ctxt, idl_type, syntax_type)


def _validate_types(ctxt, parsed_spec):
    # type: (errors.ParserContext, syntax.IDLSpec) -> None
    """Validate all types are correct."""

    for idl_type in parsed_spec.symbols.types:
        _validate_type(ctxt, idl_type)


def _bind_struct(ctxt, parsed_spec, struct):
    # type: (errors.ParserContext, syntax.IDLSpec, syntax.Struct) -> ast.Struct
    """
    Bind a struct.

    - Validating a struct and fields.
    - Create the idl.ast version from the idl.syntax tree.
    """

    ast_struct = ast.Struct(struct.file_name, struct.line, struct.column)
    ast_struct.name = struct.name
    ast_struct.description = struct.description
    ast_struct.strict = struct.strict

    # Validate naming restrictions
    if ast_struct.name.startswith("array"):
        ctxt.add_array_not_valid_error(ast_struct, "struct", ast_struct.name)

    for field in struct.fields:
        ast_field = _bind_field(ctxt, parsed_spec, field)
        if ast_field:
            ast_struct.fields.append(ast_field)

    return ast_struct


def _validate_ignored_field(ctxt, field):
    # type: (errors.ParserContext, syntax.Field) -> None
    """Validate that for ignored fields, no other properties are set."""
    if field.optional:
        ctxt.add_ignored_field_must_be_empty_error(field, field.name, "optional")
    if field.default is not None:
        ctxt.add_ignored_field_must_be_empty_error(field, field.name, "default")


def _validate_field_of_type_struct(ctxt, field):
    # type: (errors.ParserContext, syntax.Field) -> None
    """Validate that for fields with a type of struct, no other properties are set."""
    if field.default is not None:
        ctxt.add_ignored_field_must_be_empty_error(field, field.name, "default")


def _bind_field(ctxt, parsed_spec, field):
    # type: (errors.ParserContext, syntax.IDLSpec, syntax.Field) -> ast.Field
    """
    Bind a field from the idl.syntax tree.

    - Create the idl.ast version from the idl.syntax tree.
    - Validate the resulting type is correct.
    """
    ast_field = ast.Field(field.file_name, field.line, field.column)
    ast_field.name = field.name
    ast_field.description = field.description
    ast_field.optional = field.optional

    # Validate naming restrictions
    if ast_field.name.startswith("array"):
        ctxt.add_array_not_valid_error(ast_field, "field", ast_field.name)

    if field.ignore:
        ast_field.ignore = field.ignore
        _validate_ignored_field(ctxt, field)
        return ast_field

    # TODO: support array
    (struct, idltype) = parsed_spec.symbols.resolve_field_type(ctxt, field)
    if not struct and not idltype:
        return None

    # Copy over only the needed information if this a struct or a type
    if struct:
        ast_field.struct_type = struct.name
        ast_field.bson_serialization_type = ["object"]
        _validate_field_of_type_struct(ctxt, field)
    else:
        # Produce the union of type information for the type and this field.

        # Copy over the type fields first
        ast_field.cpp_type = idltype.cpp_type
        ast_field.bson_serialization_type = idltype.bson_serialization_type
        ast_field.bindata_subtype = idltype.bindata_subtype
        ast_field.serializer = idltype.serializer
        ast_field.deserializer = idltype.deserializer
        ast_field.default = idltype.default

        if field.default:
            ast_field.default = field.default

        # Validate merged type
        _validate_type_properties(ctxt, ast_field, "field")

    return ast_field


def _bind_globals(parsed_spec):
    # type: (syntax.IDLSpec) -> ast.Global
    """Bind the globals object from the idl.syntax tree into the idl.ast tree by doing a deep copy."""
    if parsed_spec.globals:
        ast_global = ast.Global(parsed_spec.globals.file_name, parsed_spec.globals.line,
                                parsed_spec.globals.column)
        ast_global.cpp_namespace = parsed_spec.globals.cpp_namespace
        ast_global.cpp_includes = parsed_spec.globals.cpp_includes
    else:
        ast_global = ast.Global("<implicit>", 0, 0)

        # If no namespace has been set, default it do "mongo"
        ast_global.cpp_namespace = "mongo"

    return ast_global


def bind(parsed_spec):
    # type: (syntax.IDLSpec) -> ast.IDLBoundSpec
    """Read an idl.syntax, create an idl.ast tree, and validate the final IDL Specification."""

    ctxt = errors.ParserContext("unknown", errors.ParserErrorCollection())

    bound_spec = ast.IDLAST()

    bound_spec.globals = _bind_globals(parsed_spec)

    _validate_types(ctxt, parsed_spec)

    for struct in parsed_spec.symbols.structs:
        bound_spec.structs.append(_bind_struct(ctxt, parsed_spec, struct))

    if ctxt.errors.has_errors():
        return ast.IDLBoundSpec(None, ctxt.errors)
    else:
        return ast.IDLBoundSpec(bound_spec, None)
