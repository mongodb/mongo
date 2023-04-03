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
"""Transform idl.syntax trees from the parser into well-defined idl.ast trees."""

import collections
import re
import typing
from typing import Type, TypeVar, cast, List, Set, Union, Optional

from . import ast
from . import bson
from . import common
from . import enum_types
from . import errors
from . import syntax


def _validate_single_bson_type(ctxt, idl_type, syntax_type):
    # type: (errors.ParserContext, Union[syntax.Type, ast.Type], str) -> bool
    """Validate bson serialization type is correct for a type."""
    bson_type = idl_type.bson_serialization_type[0]

    # Any and Chain are only valid if they are the only bson types specified
    if bson_type in ["any", "chain"]:
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
    # type: (errors.ParserContext, Union[syntax.Type, ast.Type], str) -> bool
    """Validate bson serialization type(s) is correct for a type."""

    bson_types = idl_type.bson_serialization_type
    if len(bson_types) == 1:
        return _validate_single_bson_type(ctxt, idl_type, syntax_type)

    for bson_type in bson_types:
        if bson_type in ["any", "chain"]:
            ctxt.add_bad_any_type_use_error(idl_type, bson_type, syntax_type, idl_type.name)
            return False

        if not bson.is_valid_bson_type(bson_type):
            ctxt.add_bad_bson_type_error(idl_type, syntax_type, idl_type.name, bson_type)
            return False

        if not isinstance(idl_type, syntax.VariantType):
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
    if idl_type.name.startswith("array<"):
        ctxt.add_array_not_valid_error(idl_type, "type", idl_type.name)

    _validate_type_properties(ctxt, idl_type, 'type')


def _validate_cpp_type(ctxt, idl_type, syntax_type):
    # type: (errors.ParserContext, Union[syntax.Type, ast.Type], str) -> None
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

    # Only allow 16-byte arrays since they are for MD5 and UUID
    if idl_type.cpp_type.replace(" ", "") == "std::array<std::uint8_t,16>":
        return

    # Support vector for variable length BinData.
    if idl_type.cpp_type == "std::vector<std::uint8_t>":
        return

    # Support variant for writeConcernW.
    if idl_type.cpp_type == "stdx::variant<std::string, std::int64_t>":
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


def _validate_chain_type_properties(ctxt, idl_type, syntax_type):
    # type: (errors.ParserContext, Union[syntax.Type, ast.Type], str) -> None
    """Validate a chained type has both a deserializer and serializer."""
    assert len(
        idl_type.bson_serialization_type) == 1 and idl_type.bson_serialization_type[0] == 'chain'

    if idl_type.deserializer is None:
        ctxt.add_missing_ast_required_field_error(idl_type, syntax_type, idl_type.name,
                                                  "deserializer")

    if idl_type.serializer is None:
        ctxt.add_missing_ast_required_field_error(idl_type, syntax_type, idl_type.name,
                                                  "serializer")


def _validate_type_properties(ctxt, idl_type, syntax_type):
    # type: (errors.ParserContext, Union[syntax.Type, ast.Type], str) -> None
    """Validate each type is correct."""
    # Validate bson type restrictions
    if not _validate_bson_types_list(ctxt, idl_type, syntax_type):
        return

    if len(idl_type.bson_serialization_type) == 1:
        bson_type = idl_type.bson_serialization_type[0]

        if bson_type == "any":
            # For 'any', a deserializer is required but the user can try to get away with the default
            # serialization for their C++ type.  An internal_only type is not associated with BSON
            # and thus should not have a deserializer defined.
            if idl_type.deserializer is None and not idl_type.internal_only:
                ctxt.add_missing_ast_required_field_error(idl_type, syntax_type, idl_type.name,
                                                          "deserializer")
        elif bson_type == "chain":
            _validate_chain_type_properties(ctxt, idl_type, syntax_type)

        elif bson_type == "string":
            # Strings support custom serialization unlike other non-object scalar types
            if idl_type.deserializer is None:
                ctxt.add_missing_ast_required_field_error(idl_type, syntax_type, idl_type.name,
                                                          "deserializer")

        elif not bson_type in ["array", "object", "bindata"]:
            if idl_type.deserializer is None:
                ctxt.add_missing_ast_required_field_error(idl_type, syntax_type, idl_type.name,
                                                          "deserializer")

            if idl_type.deserializer is not None and "BSONElement" not in idl_type.deserializer:
                ctxt.add_not_custom_scalar_serialization_not_supported_error(
                    idl_type, syntax_type, idl_type.name, bson_type)

            if idl_type.serializer is not None:
                ctxt.add_not_custom_scalar_serialization_not_supported_error(
                    idl_type, syntax_type, idl_type.name, bson_type)

        if bson_type == "bindata" and isinstance(idl_type, syntax.Type) and idl_type.default:
            ctxt.add_bindata_no_default(idl_type, syntax_type, idl_type.name)

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


def _is_duplicate_field(ctxt, field_container, fields, ast_field):
    # type: (errors.ParserContext, str, List[ast.Field], ast.Field) -> bool
    """Return True if there is a naming conflict for a given field."""

    # This is normally tested in the parser as part of duplicate detection in a map
    if ast_field.name in [field.name for field in fields]:
        for field in fields:
            if field.name == ast_field.name:
                duplicate_field = field

        ctxt.add_duplicate_field_error(ast_field, field_container, ast_field.name, duplicate_field)
        return True

    return False


def _get_struct_qualified_cpp_name(struct):
    # type: (syntax.Struct) -> str
    return common.qualify_cpp_name(struct.cpp_namespace,
                                   common.title_case(struct.cpp_name or struct.name))


def _bind_struct_common(ctxt, parsed_spec, struct, ast_struct):
    # type: (errors.ParserContext, syntax.IDLSpec, syntax.Struct, ast.Struct) -> None

    _inject_hidden_fields(struct)

    ast_struct.name = struct.name
    ast_struct.description = struct.description
    ast_struct.strict = struct.strict
    ast_struct.immutable = struct.immutable
    ast_struct.inline_chained_structs = struct.inline_chained_structs
    ast_struct.generate_comparison_operators = struct.generate_comparison_operators
    ast_struct.cpp_validator_func = struct.cpp_validator_func
    ast_struct.cpp_name = struct.cpp_name or struct.name
    ast_struct.qualified_cpp_name = _get_struct_qualified_cpp_name(struct)
    ast_struct.allow_global_collection_name = struct.allow_global_collection_name
    ast_struct.non_const_getter = struct.non_const_getter
    ast_struct.is_command_reply = struct.is_command_reply
    if struct.is_generic_cmd_list:
        if struct.is_generic_cmd_list == "arg":
            ast_struct.generic_list_type = ast.GenericListType.ARG
        else:
            assert struct.is_generic_cmd_list == "reply"
            ast_struct.generic_list_type = ast.GenericListType.REPLY

    # Validate naming restrictions
    if ast_struct.name.startswith("array<"):
        ctxt.add_array_not_valid_error(ast_struct, "struct", ast_struct.name)

    # Merge chained types as chained fields
    if struct.chained_types:
        if ast_struct.strict:
            ctxt.add_chained_type_no_strict_error(ast_struct, ast_struct.name)

        for chained_type in struct.chained_types:
            ast_field = _bind_chained_type(ctxt, parsed_spec, ast_struct, chained_type)
            if ast_field and not _is_duplicate_field(ctxt, chained_type.name, ast_struct.fields,
                                                     ast_field):
                ast_struct.fields.append(ast_field)

    # Merge chained structs as a chained struct and ignored fields
    for chained_struct in struct.chained_structs or []:
        _bind_chained_struct(ctxt, parsed_spec, ast_struct, chained_struct)

    # Parse the fields last so that they are serialized after chained stuff.
    for field in struct.fields or []:
        ast_field = _bind_field(ctxt, parsed_spec, field)
        if ast_field:
            if ast_struct.generic_list_type:
                gen_field_info = ast.GenericFieldInfo(struct.file_name, struct.line, struct.column)
                if ast_struct.generic_list_type == ast.GenericListType.ARG:
                    gen_field_info.forward_to_shards = field.forward_to_shards
                elif ast_struct.generic_list_type == ast.GenericListType.REPLY:
                    gen_field_info.forward_from_shards = field.forward_from_shards
                    gen_field_info.arg = False
                else:
                    assert False
                ast_field.generic_field_info = gen_field_info
            if ast_field.supports_doc_sequence and not isinstance(ast_struct, ast.Command):
                # Doc sequences are only supported in commands at the moment
                ctxt.add_bad_struct_field_as_doc_sequence_error(ast_struct, ast_struct.name,
                                                                ast_field.name)

            if ast_field.non_const_getter and struct.immutable:
                ctxt.add_bad_field_non_const_getter_in_immutable_struct_error(
                    ast_struct, ast_struct.name, ast_field.name)

            if not _is_duplicate_field(ctxt, ast_struct.name, ast_struct.fields, ast_field):
                ast_struct.fields.append(ast_field)

    # Fill out the field comparison_order property as needed
    if ast_struct.generate_comparison_operators and ast_struct.fields:
        # If the user did not specify an ordering of fields, then number all fields in
        # declared field.
        use_default_order = True
        comparison_orders = set()  # type: Set[int]

        for ast_field in ast_struct.fields:
            if not ast_field.comparison_order == -1:
                use_default_order = False
                if ast_field.comparison_order in comparison_orders:
                    ctxt.add_duplicate_comparison_order_field_error(ast_struct, ast_struct.name,
                                                                    ast_field.comparison_order)

                comparison_orders.add(ast_field.comparison_order)

        if use_default_order:
            pos = 0
            for ast_field in ast_struct.fields:
                ast_field.comparison_order = pos
                pos += 1


def _inject_hidden_fields(struct):
    # type: (syntax.Struct) -> None
    """Inject hidden fields to aid deserialization/serialization for structs."""

    # Don't generate if no fields exist or it's already included in this struct
    if struct.fields is None:
        struct.fields = []

    serialization_context_field = syntax.Field(struct.file_name, struct.line, struct.column)
    serialization_context_field.name = "serialization_context"  # This comes from basic_types.idl
    serialization_context_field.type = syntax.FieldTypeSingle(struct.file_name, struct.line,
                                                              struct.column)
    serialization_context_field.type.type_name = "serialization_context"
    serialization_context_field.cpp_name = "serializationContext"
    serialization_context_field.optional = False
    serialization_context_field.default = "SerializationContext()"

    struct.fields.append(serialization_context_field)


def _bind_struct(ctxt, parsed_spec, struct):
    # type: (errors.ParserContext, syntax.IDLSpec, syntax.Struct) -> ast.Struct
    """
    Bind a struct.

    - Validating a struct and fields.
    - Create the idl.ast version from the idl.syntax tree.
    """

    ast_struct = ast.Struct(struct.file_name, struct.line, struct.column)

    _bind_struct_common(ctxt, parsed_spec, struct, ast_struct)

    return ast_struct


def _inject_hidden_command_fields(command):
    # type: (syntax.Command) -> None
    """Inject hidden fields to aid deserialization/serialization for OpMsg parsing of commands."""

    # Inject a "$db" which we can decode during command parsing
    db_field = syntax.Field(command.file_name, command.line, command.column)
    db_field.name = "$db"
    db_field.type = syntax.FieldTypeSingle(command.file_name, command.line, command.column)
    db_field.type.type_name = "database_name"  # This comes from basic_types.idl
    db_field.cpp_name = "dbName"
    db_field.serialize_op_msg_request_only = True

    # Commands that require namespaces do not need to have db defaulted in the constructor
    if command.namespace == common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB:
        db_field.constructed = True

    command.fields.append(db_field)

    # Inject "$tenant" for use by cluster administrators overriding tenant in multitenancy.
    tenant_field = syntax.Field(command.file_name, command.line, command.column)
    tenant_field.name = "$tenant"
    tenant_field.type = syntax.FieldTypeSingle(command.file_name, command.line, command.column)
    tenant_field.type.type_name = "tenant_id"  # This comes from basic_types.idl
    tenant_field.cpp_name = "dollarTenant"
    tenant_field.optional = True
    # The $tenant field should be injected when serializing to OpMsgRequest and to
    # BSONObjBuilder if it exists.
    tenant_field.serialize_op_msg_request_only = False

    command.fields.append(tenant_field)

    # Inject "expectPrefix" used to detect whether request contains a prefixed tenantId.
    expect_prefix_field = syntax.Field(command.file_name, command.line, command.column)
    expect_prefix_field.name = "expectPrefix"
    expect_prefix_field.type = syntax.FieldTypeSingle(command.file_name, command.line,
                                                      command.column)
    expect_prefix_field.type.type_name = "bool"
    expect_prefix_field.cpp_name = "expectPrefix"
    expect_prefix_field.optional = True
    # we must extract expectPrefix before any other fields that may consume it
    expect_prefix_field.preparse = True

    command.fields.append(expect_prefix_field)


def _bind_struct_type(struct):
    # type: (syntax.Struct) -> ast.Type
    # Use Type to represent a struct-type field. (The Struct class is to generate a C++ class, not
    # represent a field's type.)
    ast_type = ast.Type(struct.file_name, struct.line, struct.column)
    ast_type.is_struct = True
    ast_type.name = struct.name
    ast_type.cpp_type = _get_struct_qualified_cpp_name(struct)
    ast_type.bson_serialization_type = ["object"]
    ast_type.first_element_field_name = struct.fields[0].name if struct.fields else None
    return ast_type


def _bind_struct_field(ctxt, ast_field, idl_type):
    # type: (errors.ParserContext, ast.Field, Union[syntax.Enum, syntax.Struct, syntax.Type]) -> None
    # The signature includes Enum to match SymbolTable.resolve_field_type, but it's not allowed.
    assert not isinstance(idl_type, syntax.Enum)
    if isinstance(idl_type, syntax.Struct):
        struct = cast(syntax.Struct, idl_type)
    else:
        assert isinstance(idl_type, syntax.ArrayType)
        array = cast(syntax.ArrayType, idl_type)
        assert isinstance(array.element_type, syntax.Struct)
        struct = cast(syntax.Struct, array.element_type)

    ast_field.type = _bind_struct_type(struct)
    ast_field.type.is_array = isinstance(idl_type, syntax.ArrayType)

    _validate_default_of_type_struct(ctxt, ast_field)


def _bind_variant_field(ctxt, ast_field, idl_type):
    # type: (errors.ParserContext, ast.Field, Union[syntax.VariantType, syntax.ArrayType]) -> None
    ast_field.type = _bind_type(idl_type)
    ast_field.type.is_variant = True

    if isinstance(idl_type, syntax.ArrayType):
        assert isinstance(idl_type.element_type, syntax.VariantType)
        idl_type = idl_type.element_type

    _validate_bson_types_list(ctxt, idl_type, "field")

    for alternative in idl_type.variant_types:
        ast_alternative = _bind_type(alternative)
        ast_field.type.variant_types.append(ast_alternative)

    if idl_type.variant_struct_types:
        ast_field.type.variant_struct_types = []

    for struct_type in idl_type.variant_struct_types:
        ast_field.type.variant_struct_types.append(_bind_struct_type(struct_type))

    def gen_cpp_types():
        for alternative in ast_field.type.variant_types:
            if alternative.is_array:
                yield f'std::vector<{alternative.cpp_type}>'
            else:
                yield alternative.cpp_type

        if ast_field.type.variant_struct_types:
            for variant_type in ast_field.type.variant_struct_types:
                yield variant_type.cpp_type

    ast_field.type.cpp_type = f'stdx::variant<{", ".join(gen_cpp_types())}>'

    # Validation doc_sequence types
    _validate_doc_sequence_field(ctxt, ast_field)


def _bind_command_type(ctxt, parsed_spec, command):
    # type: (errors.ParserContext, syntax.IDLSpec, syntax.Command) -> ast.Field
    """Bind the type field in a command as the first field."""
    ast_field = ast.Field(command.file_name, command.line, command.column)
    ast_field.name = command.name
    ast_field.description = command.description
    ast_field.optional = False
    ast_field.supports_doc_sequence = False
    ast_field.serialize_op_msg_request_only = False
    ast_field.constructed = False

    ast_field.cpp_name = "CommandParameter"

    # Validate naming restrictions
    if ast_field.name.startswith("array<"):
        ctxt.add_array_not_valid_error(ast_field, "field", ast_field.name)

    # Resolve the command type as a field
    syntax_symbol = parsed_spec.symbols.resolve_field_type(ctxt, command, command.name,
                                                           command.type)
    if syntax_symbol is None:
        return None

    if isinstance(syntax_symbol, syntax.Command):
        ctxt.add_bad_command_as_field_error(ast_field, command.type.debug_string())
        return None

    assert not isinstance(syntax_symbol, syntax.Enum)

    base_type = (syntax_symbol.element_type
                 if isinstance(syntax_symbol, syntax.ArrayType) else syntax_symbol)

    # Copy over only the needed information if this is a struct or a type.
    if isinstance(base_type, syntax.Struct):
        _bind_struct_field(ctxt, ast_field, syntax_symbol)
    elif isinstance(base_type, syntax.VariantType):
        assert isinstance(syntax_symbol, syntax.VariantType)
        _bind_variant_field(ctxt, ast_field, cast(syntax.VariantType, syntax_symbol))
    else:
        assert isinstance(base_type, syntax.Type)

        idltype = cast(syntax.Type, base_type)
        ast_field.type = _bind_type(idltype)
        ast_field.type.is_array = isinstance(syntax_symbol, syntax.ArrayType)
        ast_field.default = idltype.default

        # Validate merged type
        _validate_type_properties(ctxt, ast_field.type, "command.type")

        # Validate merged type
        _validate_field_properties(ctxt, ast_field)

    return ast_field


def _bind_command_reply_type(ctxt, parsed_spec, command):
    # type: (errors.ParserContext, syntax.IDLSpec, syntax.Command) -> ast.Field
    """Bind the reply_type field in a command."""
    ast_field = ast.Field(command.file_name, command.line, command.column)
    ast_field.name = "replyType"
    ast_field.description = f"{command.name} reply type"

    # Resolve the command type as a field
    syntax_symbol = parsed_spec.symbols.resolve_type_from_name(ctxt, command, command.name,
                                                               command.reply_type)

    if syntax_symbol is None:
        # Resolution failed, we've recorded an error.
        return None

    if not isinstance(syntax_symbol, syntax.Struct):
        ctxt.add_reply_type_invalid_type(ast_field, command.name, command.reply_type)
    else:
        ast_field.type = _bind_struct_type(syntax_symbol)
    return ast_field


def resolve_enum_value(ctxt, location, syntax_enum, name):
    # type: (errors.ParserContext, common.SourceLocation, syntax.Enum, str) -> syntax.EnumValue
    """Resolve a single enum value in an enum."""

    for value in syntax_enum.values:
        if value.value == name:
            return value

    ctxt.add_unknown_enum_value(location, syntax_enum.name, name)

    return None


def _bind_enum_value(ctxt, parsed_spec, location, enum_name, enum_value):
    # type: (errors.ParserContext, syntax.IDLSpec, common.SourceLocation, str, str) -> str

    # Look up the enum for "enum_name" in the symbol table
    access_check_enum = parsed_spec.symbols.resolve_type_from_name(ctxt, location, "access_check",
                                                                   enum_name)

    if access_check_enum is None:
        # Resolution failed, we've recorded an error.
        return None

    if not isinstance(access_check_enum, syntax.Enum):
        ctxt.add_unknown_type_error(location, enum_name, "enum")
        return None

    syntax_enum = resolve_enum_value(ctxt, location, cast(syntax.Enum, access_check_enum),
                                     enum_value)
    if not syntax_enum:
        return None

    return syntax_enum.name


def _bind_single_check(ctxt, parsed_spec, access_check):
    # type: (errors.ParserContext, syntax.IDLSpec, syntax.AccessCheck) -> ast.AccessCheck
    """Bind a single access_check."""

    ast_access_check = ast.AccessCheck(access_check.file_name, access_check.line,
                                       access_check.column)

    assert bool(access_check.check) != bool(access_check.privilege)

    if access_check.check:
        ast_access_check.check = _bind_enum_value(ctxt, parsed_spec, access_check, "AccessCheck",
                                                  access_check.check)
        if not ast_access_check.check:
            return None
    else:
        privilege = access_check.privilege
        ast_privilege = ast.Privilege(privilege.file_name, privilege.line, privilege.column)

        ast_privilege.resource_pattern = _bind_enum_value(ctxt, parsed_spec, privilege, "MatchType",
                                                          privilege.resource_pattern)
        if not ast_privilege.resource_pattern:
            return None

        ast_privilege.action_type = []
        at_names = []
        for at in privilege.action_type:
            at_names.append(at)
            bound_at = _bind_enum_value(ctxt, parsed_spec, privilege, "ActionType", at)
            if not bound_at:
                return None

            ast_privilege.action_type.append(bound_at)

        at_names_set = set(at_names)
        if len(at_names_set) != len(at_names):
            for name in at_names_set:
                if at_names.count(name) > 1:
                    ctxt.add_duplicate_action_types(ast_privilege, name)
                    return None

        ast_access_check.privilege = ast_privilege

    return ast_access_check


def _validate_check_uniqueness(ctxt, access_checks):
    # type: (errors.ParserContext, List[ast.AccessCheck]) -> bool
    """Validate there is no duplication among checks."""
    checks_set = set()
    for ac in access_checks:
        if not ac.check:
            continue

        if ac.check in checks_set:
            ctxt.add_duplicate_access_check(ac, ac.check)
            return False

        checks_set.add(ac.check)

    privs_set = set()
    for ac in access_checks:
        if not ac.privilege:
            continue

        priv = ac.privilege

        # Produce pairs of resource_pattern and action type, then de-dup them
        for at in priv.action_type:
            priv_tuple = (priv.resource_pattern, at)
            if priv_tuple in privs_set:
                ctxt.add_duplicate_access_check(ac, ac.check)
                return False

            privs_set.add(priv_tuple)

    return True


def _bind_access_check(ctxt, parsed_spec, command):
    # type: (errors.ParserContext, syntax.IDLSpec, syntax.Command) -> Optional[List[ast.AccessCheck]]
    """Bind the access_check field in a command."""

    if not command.access_check:
        return None

    access_check = command.access_check

    if access_check.none:
        return []

    if access_check.simple:
        ast_access_check = _bind_single_check(ctxt, parsed_spec, access_check.simple)
        if not ast_access_check:
            return None

        return [ast_access_check]

    if access_check.complex:
        checks = []  # List[ast.AccessCheck]
        for ac in access_check.complex:
            ast_access_check = _bind_single_check(ctxt, parsed_spec, ac)
            if not ast_access_check:
                return None
            checks.append(ast_access_check)

        if not _validate_check_uniqueness(ctxt, checks):
            return None

        return checks

    return None


def _bind_command(ctxt, parsed_spec, command):
    # type: (errors.ParserContext, syntax.IDLSpec, syntax.Command) -> ast.Command
    """
    Bind a command.

    - Validating a command and fields.
    - Create the idl.ast version from the idl.syntax tree.
    """

    ast_command = ast.Command(command.file_name, command.line, command.column)
    ast_command.api_version = command.api_version
    ast_command.is_deprecated = command.is_deprecated
    ast_command.command_name = command.command_name
    ast_command.command_alias = command.command_alias

    # Inject special fields used for command parsing
    _inject_hidden_command_fields(command)

    _bind_struct_common(ctxt, parsed_spec, command, ast_command)

    ast_command.access_checks = _bind_access_check(ctxt, parsed_spec, command)
    if command.api_version != "" and command.access_check is None:
        ctxt.add_missing_access_check(ast_command, ast_command.name)

    ast_command.namespace = command.namespace

    if command.type:
        ast_command.command_field = _bind_command_type(ctxt, parsed_spec, command)

    if command.reply_type:
        ast_command.reply_type = _bind_command_reply_type(ctxt, parsed_spec, command)

    if [field for field in ast_command.fields if field.name == ast_command.name]:
        ctxt.add_bad_command_name_duplicates_field(ast_command, ast_command.name)

    return ast_command


def _validate_ignored_field(ctxt, field):
    # type: (errors.ParserContext, syntax.Field) -> None
    """Validate that for ignored fields, no other properties are set."""
    if field.optional:
        ctxt.add_ignored_field_must_be_empty_error(field, field.name, "optional")
    if field.default is not None:
        ctxt.add_ignored_field_must_be_empty_error(field, field.name, "default")


def _validate_default_of_type_struct(ctxt, field):
    # type: (errors.ParserContext, Union[syntax.Field, ast.Field]) -> None
    """Validate that for fields with a type of struct, the only default permitted is true, which causes it to be default-constructed."""
    if (field.default is not None) and (field.default != "true"):
        ctxt.add_struct_default_must_be_true_or_empty_error(field, field.name)


def _validate_variant_type(ctxt, syntax_symbol, field):
    # type: (errors.ParserContext, syntax.VariantType, syntax.Field) -> None
    """Validate that this field is a proper variant type."""

    # Check for duplicate BSON serialization types.
    type_count: typing.Counter[str] = collections.Counter()
    array_type_count: typing.Counter[str] = collections.Counter()

    def add_to_count(counter, bson_serialization_type):
        # type: (typing.Counter[str], List[str]) -> None
        for the_type in bson_serialization_type:
            counter[the_type] += 1

    for alternative in syntax_symbol.variant_types:
        # Impossible: there's no IDL syntax for expressing nested variants.
        assert not isinstance(alternative, syntax.VariantType), "Nested variant types"
        if isinstance(alternative, syntax.ArrayType):
            if isinstance(alternative.element_type, syntax.Type):
                element_type = cast(syntax.Type, alternative.element_type)
                add_to_count(array_type_count, element_type.bson_serialization_type)
            else:
                assert isinstance(alternative.element_type, syntax.Struct)
                add_to_count(array_type_count, ["object"])
        else:
            add_to_count(type_count, alternative.bson_serialization_type)

    if syntax_symbol.variant_struct_types:
        type_count["object"] += 1

    for type_name, count in type_count.items():
        if count > 1:
            ctxt.add_variant_duplicate_types_error(syntax_symbol, field.name, type_name)

    for type_name, count in array_type_count.items():
        if count > 1:
            ctxt.add_variant_duplicate_types_error(syntax_symbol, field.name, f'array<{type_name}>')

    types = len(syntax_symbol.variant_types) + len(syntax_symbol.variant_struct_types)
    if types < 2:
        ctxt.add_useless_variant_error(syntax_symbol)


def _validate_array_type(ctxt, syntax_symbol, field):
    # type: (errors.ParserContext, syntax.ArrayType, syntax.Field) -> None
    """Validate this an array of plain objects or a struct."""
    elem_type = syntax_symbol.element_type
    if field.default or isinstance(elem_type, syntax.Type) and elem_type.default:
        ctxt.add_array_no_default_error(field, field.name)


def _validate_field_properties(ctxt, ast_field):
    # type: (errors.ParserContext, ast.Field) -> None
    """Validate field specific rules."""

    if ast_field.default:
        if ast_field.optional:
            ctxt.add_bad_field_default_and_optional(ast_field, ast_field.name)

        if ast_field.type.bson_serialization_type == ['bindata']:
            ctxt.add_bindata_no_default(ast_field, ast_field.type.name, ast_field.name)

    if ast_field.always_serialize and not ast_field.optional:
        ctxt.add_bad_field_always_serialize_not_optional(ast_field, ast_field.name)

    # A "chain" type should never appear as a field.
    if ast_field.type.bson_serialization_type == ['chain']:
        ctxt.add_bad_array_of_chain(ast_field, ast_field.name)


def _validate_doc_sequence_field(ctxt, ast_field):
    # type: (errors.ParserContext, ast.Field) -> None
    """Validate the doc_sequence is an array of plain objects."""
    if not ast_field.supports_doc_sequence:
        return

    assert ast_field.type.is_array

    # The only allowed BSON type for a doc_sequence field is "object"
    for serialization_type in ast_field.type.bson_serialization_type:
        if serialization_type != 'object':
            ctxt.add_bad_non_object_as_doc_sequence_error(ast_field, ast_field.name)


def _normalize_method_name(cpp_type_name, cpp_method_name):
    # type: (str, str) -> str
    """Normalize the method name to be fully-qualified with the type name."""
    # Default deserializer
    if not cpp_method_name:
        return cpp_method_name

    # Global function
    if cpp_method_name.startswith('::'):
        return cpp_method_name

    # Method is full qualified already
    if cpp_method_name.startswith(cpp_type_name):
        return cpp_method_name

    # Get the unqualified type name
    type_name = cpp_type_name.split("::")[-1]

    # Method is prefixed with just the type name
    if cpp_method_name.startswith(type_name):
        return '::'.join(cpp_type_name.split('::')[0:-1]) + "::" + cpp_method_name

    return cpp_method_name


def _bind_expression(expr, allow_literal_string=True):
    # type: (syntax.Expression, bool) -> ast.Expression
    """Bind an expression."""
    node = ast.Expression(expr.file_name, expr.line, expr.column)

    if expr.literal is None:
        node.expr = expr.expr
        node.validate_constexpr = expr.is_constexpr
        node.export = expr.is_constexpr
        return node

    node.validate_constexpr = False
    node.export = True

    # bool
    if (expr.literal == "true") or (expr.literal == "false"):
        node.expr = expr.literal
        return node

    # int32_t
    try:
        intval = int(expr.literal)
        if intval >= -0x80000000 and intval <= 0x7FFFFFFF:  # pylint: disable=chained-comparison
            node.expr = repr(intval)
            return node
    except ValueError:
        pass

    # float
    try:
        node.expr = repr(float(expr.literal))
        return node
    except ValueError:
        pass

    # std::string
    if allow_literal_string:
        strval = expr.literal
        for i in ['\\', '"', "'"]:
            if i in strval:
                strval = strval.replace(i, '\\' + i)
        node.expr = '"' + strval + '"'
        return node

    # Unable to bind expression.
    return None


def _bind_validator(ctxt, validator):
    # type: (errors.ParserContext, syntax.Validator) -> ast.Validator
    """Bind a validator from the idl.syntax tree."""

    ast_validator = ast.Validator(validator.file_name, validator.line, validator.column)

    # Parse syntax value as numeric if possible.
    for pred in ["gt", "lt", "gte", "lte"]:
        src = getattr(validator, pred)
        if src is None:
            continue

        dest = _bind_expression(src, allow_literal_string=False)
        if dest is None:
            # This only happens if we have a non-numeric literal.
            ctxt.add_value_not_numeric_error(ast_validator, pred, src)
            return None

        setattr(ast_validator, pred, dest)

    ast_validator.callback = validator.callback
    return ast_validator


def _bind_condition(condition, condition_for):
    # type: (syntax.Condition, str) -> ast.Condition
    """Bind a condition from the idl.syntax tree."""

    if not condition:
        return None

    ast_condition = ast.Condition(condition.file_name, condition.line, condition.column)
    ast_condition.expr = condition.expr
    ast_condition.constexpr = condition.constexpr
    ast_condition.preprocessor = condition.preprocessor

    if condition.feature_flag:
        assert condition_for == 'server_parameter'
        ast_condition.feature_flag = condition.feature_flag

    if condition.min_fcv:
        assert condition_for == 'server_parameter'
        ast_condition.min_fcv = condition.min_fcv

    return ast_condition


def _bind_type(idltype):
    # type: (syntax.Type) -> ast.Type
    """Bind a type."""
    if isinstance(idltype, syntax.ArrayType):
        if isinstance(idltype.element_type, syntax.Struct):
            ast_type = _bind_struct_type(cast(syntax.Struct, idltype.element_type))
        else:
            assert isinstance(idltype.element_type, syntax.Type)
            ast_type = _bind_type(idltype.element_type)

        ast_type.is_array = True
        return ast_type

    ast_type = ast.Type(idltype.file_name, idltype.line, idltype.column)
    ast_type.name = idltype.name
    ast_type.cpp_type = idltype.cpp_type
    ast_type.bson_serialization_type = idltype.bson_serialization_type
    ast_type.bindata_subtype = idltype.bindata_subtype
    ast_type.serializer = _normalize_method_name(idltype.cpp_type, idltype.serializer)
    ast_type.deserializer = _normalize_method_name(idltype.cpp_type, idltype.deserializer)
    ast_type.deserialize_with_tenant = idltype.deserialize_with_tenant
    ast_type.internal_only = idltype.internal_only
    return ast_type


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
    ast_field.supports_doc_sequence = field.supports_doc_sequence
    ast_field.serialize_op_msg_request_only = field.serialize_op_msg_request_only
    ast_field.constructed = field.constructed
    ast_field.comparison_order = field.comparison_order
    ast_field.non_const_getter = field.non_const_getter
    # Ignore the 'unstable' field since it's deprecated by the 'stability' field and only there at parsing level
    # to provide compatibility support.
    ast_field.stability = field.stability
    ast_field.always_serialize = field.always_serialize
    ast_field.preparse = field.preparse

    ast_field.cpp_name = field.name
    if field.cpp_name:
        ast_field.cpp_name = field.cpp_name

    # Validate naming restrictions
    if ast_field.name.startswith("array<"):
        ctxt.add_array_not_valid_error(ast_field, "field", ast_field.name)

    if field.ignore:
        ast_field.ignore = field.ignore
        _validate_ignored_field(ctxt, field)
        return ast_field

    syntax_symbol = parsed_spec.symbols.resolve_field_type(ctxt, field, field.name, field.type)
    if syntax_symbol is None:
        return None

    ast_field.default = field.default

    if isinstance(syntax_symbol, syntax.Command):
        ctxt.add_bad_command_as_field_error(ast_field, field.type.debug_string())
        return None

    if isinstance(syntax_symbol, syntax.VariantType):
        _validate_variant_type(ctxt, cast(syntax.VariantType, syntax_symbol), field)

    if isinstance(syntax_symbol, syntax.ArrayType):
        _validate_array_type(ctxt, cast(syntax.ArrayType, syntax_symbol), field)
    elif field.supports_doc_sequence:
        # Doc sequences are only supported for arrays
        ctxt.add_bad_non_array_as_doc_sequence_error(syntax_symbol, syntax_symbol.name,
                                                     ast_field.name)
        return None

    base_type = (syntax_symbol.element_type
                 if isinstance(syntax_symbol, syntax.ArrayType) else syntax_symbol)

    # Copy over only the needed information if this is a struct or a type.

    if isinstance(base_type, syntax.Struct):
        _bind_struct_field(ctxt, ast_field, syntax_symbol)
    elif isinstance(base_type, syntax.Enum):
        ast_field.type = ast.Type(base_type.file_name, base_type.line, base_type.column)
        ast_field.type.name = base_type.name
        ast_field.type.is_enum = True

        enum_type_info = enum_types.get_type_info(cast(syntax.Enum, base_type))
        ast_field.type.cpp_type = enum_type_info.get_qualified_cpp_type_name()
        ast_field.type.bson_serialization_type = enum_type_info.get_bson_types()
        ast_field.type.serializer = enum_type_info.get_enum_serializer_name()
        ast_field.type.deserializer = enum_type_info.get_enum_deserializer_name()
    elif isinstance(base_type, syntax.VariantType):
        # syntax_symbol is an Array for arrays of variant.
        assert isinstance(syntax_symbol, (syntax.ArrayType, syntax.VariantType))
        _bind_variant_field(ctxt, ast_field, syntax_symbol)
    else:
        assert isinstance(base_type, syntax.Type)

        idltype = cast(syntax.Type, base_type)
        ast_field.type = _bind_type(idltype)
        ast_field.type.is_array = isinstance(syntax_symbol, syntax.ArrayType)
        ast_field.default = idltype.default

        if field.default:
            ast_field.default = field.default

        # Validate merged type
        _validate_type_properties(ctxt, ast_field.type, "field")

        # Validate merged type
        _validate_field_properties(ctxt, ast_field)

        # Validation doc_sequence types
        _validate_doc_sequence_field(ctxt, ast_field)

    if field.validator is not None:
        ast_field.validator = _bind_validator(ctxt, field.validator)
        if ast_field.validator is None:
            return None

    return ast_field


def _bind_chained_type(ctxt, parsed_spec, location, chained_type):
    # type: (errors.ParserContext, syntax.IDLSpec, common.SourceLocation, syntax.ChainedType) -> ast.Field
    """Bind the specified chained type."""
    syntax_symbol = parsed_spec.symbols.resolve_type_from_name(ctxt, location, chained_type.name,
                                                               chained_type.name)
    if not syntax_symbol:
        return None

    if not isinstance(syntax_symbol, syntax.Type):
        ctxt.add_chained_type_not_found_error(location, chained_type.name)
        return None

    idltype = cast(syntax.Type, syntax_symbol)

    if len(idltype.bson_serialization_type) != 1 or idltype.bson_serialization_type[0] != 'chain':
        ctxt.add_chained_type_wrong_type_error(location, chained_type.name,
                                               idltype.bson_serialization_type[0])
        return None

    ast_field = ast.Field(location.file_name, location.line, location.column)
    ast_field.name = idltype.name
    ast_field.cpp_name = chained_type.cpp_name
    ast_field.description = idltype.description
    ast_field.chained = True
    ast_field.type = _bind_type(idltype)

    return ast_field


def _bind_chained_struct(ctxt, parsed_spec, ast_struct, chained_struct):
    # type: (errors.ParserContext, syntax.IDLSpec, ast.Struct, syntax.ChainedStruct) -> None
    """Bind the specified chained struct."""
    syntax_symbol = parsed_spec.symbols.resolve_type_from_name(
        ctxt, ast_struct, chained_struct.name, chained_struct.name)

    if not syntax_symbol:
        return

    if not isinstance(syntax_symbol, syntax.Struct) or isinstance(syntax_symbol, syntax.Command):
        ctxt.add_chained_struct_not_found_error(ast_struct, chained_struct.name)
        return

    struct = cast(syntax.Struct, syntax_symbol)

    # chained struct cannot be strict unless it is inlined
    if struct.strict and not ast_struct.inline_chained_structs:
        ctxt.add_chained_nested_struct_no_strict_error(ast_struct, ast_struct.name,
                                                       chained_struct.name)

    if struct.chained_types or struct.chained_structs:
        ctxt.add_chained_nested_struct_no_nested_error(ast_struct, ast_struct.name,
                                                       chained_struct.name)

    # Configure a field for the chained struct.
    ast_chained_field = ast.Field(ast_struct.file_name, ast_struct.line, ast_struct.column)
    ast_chained_field.name = struct.name
    ast_chained_field.type = _bind_struct_type(struct)
    ast_chained_field.cpp_name = chained_struct.cpp_name
    ast_chained_field.description = struct.description
    ast_chained_field.chained = True

    if not _is_duplicate_field(ctxt, chained_struct.name, ast_struct.fields, ast_chained_field):
        ast_struct.fields.append(ast_chained_field)
    else:
        return

    # Merge all the fields from resolved struct into this ast struct.
    for field in struct.fields or []:
        ast_field = _bind_field(ctxt, parsed_spec, field)
        # Don't use internal fields in chained types, stick to local access only
        if ast_field.type.internal_only:
            continue
        if ast_field and not _is_duplicate_field(ctxt, chained_struct.name, ast_struct.fields,
                                                 ast_field):

            if ast_struct.inline_chained_structs:
                ast_field.chained_struct_field = ast_chained_field
            else:
                # For non-inlined structs, mark them as ignore
                ast_field.ignore = True

            ast_struct.fields.append(ast_field)


def _bind_globals(parsed_spec):
    # type: (syntax.IDLSpec) -> ast.Global
    """Bind the globals object from the idl.syntax tree into the idl.ast tree by doing a deep copy."""
    if parsed_spec.globals:
        ast_global = ast.Global(parsed_spec.globals.file_name, parsed_spec.globals.line,
                                parsed_spec.globals.column)
        ast_global.cpp_namespace = parsed_spec.globals.cpp_namespace
        ast_global.cpp_includes = parsed_spec.globals.cpp_includes

        configs = parsed_spec.globals.configs
        if configs:
            ast_global.configs = ast.ConfigGlobal(configs.file_name, configs.line, configs.column)

            if configs.initializer:
                init = configs.initializer

                ast_global.configs.initializer = ast.GlobalInitializer(
                    init.file_name, init.line, init.column)
                # Parser rule makes it impossible to have both name and register/store.
                ast_global.configs.initializer.name = init.name
                ast_global.configs.initializer.register = init.register
                ast_global.configs.initializer.store = init.store

    else:
        ast_global = ast.Global("<implicit>", 0, 0)

        # If no namespace has been set, default it do "mongo"
        ast_global.cpp_namespace = "mongo"

    return ast_global


def _validate_enum_int(ctxt, idl_enum):
    # type: (errors.ParserContext, syntax.Enum) -> None
    """Validate an integer enumeration."""

    # Check they are all ints
    int_values_set = set()  # type: Set[int]

    for enum_value in idl_enum.values:
        try:
            int_values_set.add(int(enum_value.value))
        except ValueError as value_error:
            ctxt.add_enum_value_not_int_error(idl_enum, idl_enum.name, enum_value.value,
                                              str(value_error))
            return

    # Check the values are continuous so they can be static_cast.
    min_value = min(int_values_set)
    max_value = max(int_values_set)

    valid_int = set(range(min_value, max_value + 1))

    if valid_int != int_values_set:
        ctxt.add_enum_non_continuous_range_error(idl_enum, idl_enum.name)


def _bind_enum(ctxt, idl_enum):
    # type: (errors.ParserContext, syntax.Enum) -> ast.Enum
    """
    Bind an enum.

    - Validating an enum and values.
    - Create the idl.ast version from the idl.syntax tree.
    """

    ast_enum = ast.Enum(idl_enum.file_name, idl_enum.line, idl_enum.column)
    ast_enum.name = idl_enum.name
    ast_enum.description = idl_enum.description
    ast_enum.type = idl_enum.type
    ast_enum.cpp_namespace = idl_enum.cpp_namespace

    enum_type_info = enum_types.get_type_info(idl_enum)
    if not enum_type_info:
        ctxt.add_enum_bad_type_error(idl_enum, idl_enum.name, idl_enum.type)
        return None

    for enum_value in idl_enum.values:
        ast_enum_value = ast.EnumValue(enum_value.file_name, enum_value.line, enum_value.column)
        ast_enum_value.name = enum_value.name
        ast_enum_value.description = enum_value.description
        ast_enum_value.value = enum_value.value
        ast_enum_value.extra_data = enum_value.extra_data
        ast_enum.values.append(ast_enum_value)

    values_set = set()  # type: Set[str]
    for enum_value in idl_enum.values:
        values_set.add(enum_value.value)

    # Check the values are unique
    if len(idl_enum.values) != len(values_set):
        ctxt.add_enum_value_not_unique_error(idl_enum, idl_enum.name)

    if ast_enum.type == 'int':
        _validate_enum_int(ctxt, idl_enum)

    return ast_enum


def _bind_server_parameter_class(ctxt, ast_param, param):
    # type: (errors.ParserContext, ast.ServerParameter, syntax.ServerParameter) -> ast.ServerParameter
    """Bind and validate ServerParameter attributes specific to specialized ServerParameters."""

    # Fields specific to bound and unbound standard params.
    for field in ['cpp_vartype', 'cpp_varname', 'on_update', 'validator']:
        if getattr(param, field) is not None:
            ctxt.add_server_parameter_invalid_attr(param, field, 'specialized')
            return None

    # Fields specific to specialized stroage.
    cls = param.cpp_class

    if param.default is not None:
        if not param.default.is_constexpr:
            ctxt.add_server_parameter_invalid_attr(param, 'default.is_constexpr=false',
                                                   'specialized')
            return None

        ast_param.default = _bind_expression(param.default)
        if ast_param.default is None:
            return None

    ast_param.cpp_class = ast.ServerParameterClass(cls.file_name, cls.line, cls.column)
    ast_param.cpp_class.name = cls.name
    ast_param.cpp_class.data = cls.data
    ast_param.cpp_class.override_ctor = cls.override_ctor
    ast_param.cpp_class.override_validate = cls.override_validate

    # If set_at is cluster, then set must be overridden. Otherwise, use the parsed value.
    ast_param.cpp_class.override_set = True if param.set_at == ['cluster'] else cls.override_set

    return ast_param


def _bind_server_parameter_with_storage(ctxt, ast_param, param):
    # type: (errors.ParserContext, ast.ServerParameter, syntax.ServerParameter) -> ast.ServerParameter
    """Bind and validate ServerParameter attributes specific to bound ServerParameters."""

    # Fields specific to specialized and unbound standard params.
    for field in ['cpp_class']:
        if getattr(param, field) is not None:
            ctxt.add_server_parameter_invalid_attr(param, field, 'bound')
            return None

    if param.set_at == ['cluster']:
        ast_param.cpp_vartype = f'TenantIdMap<{param.cpp_vartype}>'
    else:
        ast_param.cpp_vartype = param.cpp_vartype
    ast_param.cpp_varname = param.cpp_varname
    ast_param.on_update = param.on_update

    if param.default:
        ast_param.default = _bind_expression(param.default)
        if ast_param.default is None:
            return None

    if param.validator:
        ast_param.validator = _bind_validator(ctxt, param.validator)
        if ast_param.validator is None:
            return None

    return ast_param


def _bind_server_parameter_set_at(ctxt, param):
    # type: (errors.ParserContext, syntax.ServerParameter) -> str
    """Translate set_at options to C++ enum value."""

    if param.set_at == ['readonly']:
        # Readonly may not be mixed with startup or runtime
        return "ServerParameterType::kReadOnly"

    if param.set_at == ['cluster']:
        # Cluster-wide parameters may not be mixed with startup or runtime.
        # They are implicitly runtime-only.
        return "ServerParameterType::kClusterWide"

    set_at = 0
    for psa in param.set_at:
        if psa.lower() == 'startup':
            set_at |= 1
        elif psa.lower() == 'runtime':
            set_at |= 2
        else:
            ctxt.add_bad_setat_specifier(param, psa)
            return None

    mask_to_text = {
        1: "ServerParameterType::kStartupOnly",
        2: "ServerParameterType::kRuntimeOnly",
        3: "ServerParameterType::kStartupAndRuntime",
    }

    if set_at in mask_to_text:
        return mask_to_text[set_at]

    # Can't happen based on above logic.
    ctxt.add_bad_setat_specifier(param, ','.join(param.set_at))
    return None


def _bind_server_parameter(ctxt, param):
    # type: (errors.ParserContext, syntax.ServerParameter) -> ast.ServerParameter
    """Bind a serverParameter setting."""
    ast_param = ast.ServerParameter(param.file_name, param.line, param.column)
    ast_param.name = param.name
    ast_param.description = param.description
    ast_param.condition = _bind_condition(param.condition, condition_for='server_parameter')
    ast_param.redact = param.redact
    ast_param.test_only = param.test_only
    ast_param.deprecated_name = param.deprecated_name

    ast_param.set_at = _bind_server_parameter_set_at(ctxt, param)
    if ast_param.set_at is None:
        return None

    if param.cpp_class:
        return _bind_server_parameter_class(ctxt, ast_param, param)
    elif param.cpp_varname:
        return _bind_server_parameter_with_storage(ctxt, ast_param, param)
    else:
        ctxt.add_server_parameter_required_attr(param, 'cpp_varname', 'server_parameter')
        return None


def _bind_feature_flags(ctxt, param):
    # type: (errors.ParserContext, syntax.FeatureFlag) -> ast.ServerParameter
    """Bind a FeatureFlag as a serverParameter setting."""
    ast_param = ast.ServerParameter(param.file_name, param.line, param.column)
    ast_param.name = param.name
    ast_param.description = param.description

    ast_param.set_at = "ServerParameterType::kStartupOnly"

    ast_param.cpp_vartype = "::mongo::FeatureFlag"

    # Feature flags that default to false must not have a version
    if param.default.literal == "false" and param.version:
        ctxt.add_feature_flag_default_false_has_version(param)
        return None

    # Feature flags that default to true are required to have a version
    if param.default.literal == "true" and not param.version:
        ctxt.add_feature_flag_default_true_missing_version(param)
        return None

    expr = syntax.Expression(param.default.file_name, param.default.line, param.default.column)
    expr.expr = '%s, "%s"_sd' % (param.default.literal, param.version if param.version else '')

    ast_param.default = _bind_expression(expr)
    ast_param.default.export = False
    ast_param.cpp_varname = param.cpp_varname
    ast_param.feature_flag = True

    return ast_param


def _is_invalid_config_short_name(name):
    # type: (str) -> bool
    """Check if a given name is valid as a short name."""
    return ('.' in name) or (',' in name)


def _parse_config_option_sources(source_list):
    # type: (List[str]) -> str
    """Parse source list into enum value used by runtime."""
    sources = 0
    if not source_list:
        return None

    for source in source_list:
        if source == "cli":
            sources |= 1
        elif source == "ini":
            sources |= 2
        elif source == "yaml":
            sources |= 4
        else:
            return None

    source_map = [
        "SourceCommandLine",
        "SourceINIConfig",
        "SourceAllLegacy",  # cli + ini
        "SourceYAMLConfig",
        "SourceYAMLCLI",  # cli + yaml
        "SourceAllConfig",  # ini + yaml
        "SourceAll",
    ]
    return source_map[sources - 1]


def _bind_config_option(ctxt, globals_spec, option):
    # type: (errors.ParserContext, syntax.Global, syntax.ConfigOption) -> ast.ConfigOption
    """Bind a config setting."""

    node = ast.ConfigOption(option.file_name, option.line, option.column)

    if _is_invalid_config_short_name(option.short_name or ''):
        ctxt.add_invalid_short_name(option, option.short_name)
        return None

    for name in option.deprecated_short_name:
        if _is_invalid_config_short_name(name):
            ctxt.add_invalid_short_name(option, name)
            return None

    if option.single_name is not None:
        if (len(option.single_name) != 1) or not option.single_name.isalpha():
            ctxt.add_invalid_single_name(option, option.single_name)
            return None

    node.name = option.name
    node.short_name = option.short_name
    node.deprecated_name = option.deprecated_name
    node.deprecated_short_name = option.deprecated_short_name

    if (node.short_name is None) and not _is_invalid_config_short_name(node.name):
        # If the "dotted name" is usable as a "short name", mirror it by default.
        node.short_name = node.name

    if option.single_name:
        # Compose short_name/single_name into boost::program_options format.
        if not node.short_name:
            ctxt.add_missing_short_name_with_single_name(option, option.single_name)
            return None

        node.short_name = node.short_name + ',' + option.single_name

    node.description = _bind_expression(option.description)
    node.arg_vartype = option.arg_vartype
    node.cpp_vartype = option.cpp_vartype
    node.cpp_varname = option.cpp_varname
    node.condition = _bind_condition(option.condition, condition_for='config')

    node.requires = option.requires
    node.conflicts = option.conflicts
    node.hidden = option.hidden
    node.redact = option.redact
    node.canonicalize = option.canonicalize

    if option.default:
        node.default = _bind_expression(option.default)

    if option.implicit:
        node.implicit = _bind_expression(option.implicit)

    # Commonly repeated attributes section and source may be set in globals.
    if globals_spec and globals_spec.configs:
        node.section = option.section or globals_spec.configs.section
        source_list = option.source or globals_spec.configs.source or []
    else:
        node.section = option.section
        source_list = option.source or []

    node.source = _parse_config_option_sources(source_list)
    if node.source is None:
        ctxt.add_bad_source_specifier(option, ', '.join(source_list))
        return None

    if option.duplicate_behavior:
        if option.duplicate_behavior == "append":
            node.duplicates_append = True
        elif option.duplicate_behavior != "overwrite":
            ctxt.add_bad_duplicate_behavior(option, option.duplicate_behavior)
            return None

    if option.positional:
        if not node.short_name:
            ctxt.add_missing_shortname_for_positional_arg(option)
            return None

        # Parse single digit, closed range, or open range of digits.
        spread = option.positional.split('-')
        if len(spread) == 1:
            # Make a single number behave like a range of that number, (e.g. "2" -> "2-2").
            spread.append(spread[0])
        if (len(spread) != 2) or ((spread[0] == "") and (spread[1] == "")):
            ctxt.add_bad_numeric_range(option, 'positional', option.positional)
        try:
            node.positional_start = int(spread[0] or "-1")
            node.positional_end = int(spread[1] or "-1")
        except ValueError:
            ctxt.add_bad_numeric_range(option, 'positional', option.positional)
            return None

    if option.validator is not None:
        node.validator = _bind_validator(ctxt, option.validator)
        if node.validator is None:
            return None

    return node


def bind(parsed_spec):
    # type: (syntax.IDLSpec) -> ast.IDLBoundSpec
    """Read an idl.syntax, create an idl.ast tree, and validate the final IDL Specification."""

    ctxt = errors.ParserContext("unknown", errors.ParserErrorCollection())

    bound_spec = ast.IDLAST()

    bound_spec.globals = _bind_globals(parsed_spec)

    _validate_types(ctxt, parsed_spec)

    # Check enums before structs to ensure they are valid
    for idl_enum in parsed_spec.symbols.enums:
        if not idl_enum.imported:
            bound_spec.enums.append(_bind_enum(ctxt, idl_enum))

    for command in parsed_spec.symbols.commands:
        if not command.imported:
            bound_spec.commands.append(_bind_command(ctxt, parsed_spec, command))

    for struct in parsed_spec.symbols.structs:
        if not struct.imported:
            bound_spec.structs.append(_bind_struct(ctxt, parsed_spec, struct))

    for feature_flag in parsed_spec.feature_flags:
        bound_spec.server_parameters.append(_bind_feature_flags(ctxt, feature_flag))

    for server_parameter in parsed_spec.server_parameters:
        bound_spec.server_parameters.append(_bind_server_parameter(ctxt, server_parameter))

    for option in parsed_spec.configs:
        bound_spec.configs.append(_bind_config_option(ctxt, parsed_spec.globals, option))

    if ctxt.errors.has_errors():
        return ast.IDLBoundSpec(None, ctxt.errors)

    return ast.IDLBoundSpec(bound_spec, None)
