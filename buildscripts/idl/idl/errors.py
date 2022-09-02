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
Common error handling code for IDL compiler.

- Common Exceptions used by IDL compiler.
- Error codes used by the IDL compiler.
"""

import inspect
import os
import sys
from typing import List, Union
import yaml

from . import common

# Public error Codes used by IDL Compiler.
# Used by tests cases to validate expected errors are thrown in negative tests.
# Error codes must be unique, validated  _assert_unique_error_messages on file load.
#
ERROR_ID_UNKNOWN_ROOT = "ID0001"
ERROR_ID_DUPLICATE_SYMBOL = "ID0002"
ERROR_ID_IS_NODE_TYPE = "ID0003"
ERROR_ID_IS_NODE_TYPE_SCALAR_OR_SEQUENCE = "ID0004"
ERROR_ID_DUPLICATE_NODE = "ID0005"
ERROR_ID_UNKNOWN_TYPE = "ID0006"
ERROR_ID_IS_NODE_VALID_BOOL = "ID0007"
ERROR_ID_UNKNOWN_NODE = "ID0008"
ERROR_ID_MISSING_REQUIRED_FIELD = "ID0010"
ERROR_ID_ARRAY_NOT_VALID_TYPE = "ID0011"
ERROR_ID_MISSING_AST_REQUIRED_FIELD = "ID0012"
ERROR_ID_BAD_BSON_TYPE = "ID0013"
ERROR_ID_BAD_BSON_TYPE_LIST = "ID0014"
ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_TYPE = "ID0015"
ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE = "ID0016"
ERROR_ID_NO_STRINGDATA = "ID0017"
ERROR_ID_FIELD_MUST_BE_EMPTY_FOR_IGNORED = "ID0018"
ERROR_ID_DEFAULT_MUST_BE_TRUE_OR_EMPTY_FOR_STRUCT = "ID0019"
ERROR_ID_CUSTOM_SCALAR_SERIALIZATION_NOT_SUPPORTED = "ID0020"
ERROR_ID_BAD_ANY_TYPE_USE = "ID0021"
ERROR_ID_BAD_NUMERIC_CPP_TYPE = "ID0022"
ERROR_ID_BAD_ARRAY_TYPE_NAME = "ID0023"
ERROR_ID_ARRAY_NO_DEFAULT = "ID0024"
ERROR_ID_BAD_IMPORT = "ID0025"
ERROR_ID_BAD_BINDATA_DEFAULT = "ID0026"
ERROR_ID_CHAINED_TYPE_NOT_FOUND = "ID0027"
ERROR_ID_CHAINED_TYPE_WRONG_BSON_TYPE = "ID0028"
ERROR_ID_CHAINED_DUPLICATE_FIELD = "ID0029"
ERROR_ID_CHAINED_NO_TYPE_STRICT = "ID0030"
ERROR_ID_CHAINED_STRUCT_NOT_FOUND = "ID0031"
ERROR_ID_CHAINED_NO_NESTED_STRUCT_STRICT = "ID0032"
ERROR_ID_CHAINED_NO_NESTED_CHAINED = "ID0033"
ERROR_ID_BAD_EMPTY_ENUM = "ID0034"
ERROR_ID_NO_ARRAY_ENUM = "ID0035"
ERROR_ID_ENUM_BAD_TYPE = "ID0036"
ERROR_ID_ENUM_BAD_INT_VAUE = "ID0037"
ERROR_ID_ENUM_NON_UNIQUE_VALUES = "ID0038"
ERROR_ID_ENUM_NON_CONTINUOUS_RANGE = "ID0039"
ERROR_ID_BAD_COMMAND_NAMESPACE = "ID0041"
ERROR_ID_FIELD_NO_COMMAND = "ID0042"
ERROR_ID_NO_ARRAY_OF_CHAIN = "ID0043"
ERROR_ID_ILLEGAL_FIELD_DEFAULT_AND_OPTIONAL = "ID0044"
ERROR_ID_STRUCT_NO_DOC_SEQUENCE = "ID0045"
ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_ARRAY = "ID0046"
ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_OBJECT = "ID0047"
ERROR_ID_COMMAND_DUPLICATES_FIELD = "ID0048"
ERROR_ID_IS_NODE_VALID_INT = "ID0049"
ERROR_ID_IS_NODE_VALID_NON_NEGATIVE_INT = "ID0050"
ERROR_ID_IS_DUPLICATE_COMPARISON_ORDER = "ID0051"
ERROR_ID_IS_COMMAND_TYPE_EXTRANEOUS = "ID0052"
ERROR_ID_VALUE_NOT_NUMERIC = "ID0053"
ERROR_ID_BAD_SETAT_SPECIFIER = "ID0057"
ERROR_ID_BAD_SOURCE_SPECIFIER = "ID0058"
ERROR_ID_BAD_DUPLICATE_BEHAVIOR_SPECIFIER = "ID0059"
ERROR_ID_BAD_NUMERIC_RANGE = "ID0060"
ERROR_ID_MISSING_SHORTNAME_FOR_POSITIONAL = "ID0061"
ERROR_ID_INVALID_SHORT_NAME = "ID0062"
ERROR_ID_INVALID_SINGLE_NAME = "ID0063"
ERROR_ID_MISSING_SHORT_NAME_WITH_SINGLE_NAME = "ID0064"
ERROR_ID_IS_NODE_TYPE_SCALAR_OR_MAPPING = "ID0065"
ERROR_ID_SERVER_PARAMETER_INVALID_ATTR = "ID0066"
ERROR_ID_SERVER_PARAMETER_REQUIRED_ATTR = "ID0067"
ERROR_ID_SERVER_PARAMETER_INVALID_METHOD_OVERRIDE = "ID0068"
ERROR_ID_NON_CONST_GETTER_IN_IMMUTABLE_STRUCT = "ID0069"
ERROR_ID_FEATURE_FLAG_DEFAULT_TRUE_MISSING_VERSION = "ID0070"
ERROR_ID_FEATURE_FLAG_DEFAULT_FALSE_HAS_VERSION = "ID0071"
ERROR_ID_INVALID_REPLY_TYPE = "ID0072"
ERROR_ID_STABILITY_NO_API_VERSION = "ID0073"
ERROR_ID_MISSING_REPLY_TYPE = "ID0074"
ERROR_ID_USELESS_VARIANT = "ID0076"
ERROR_ID_ILLEGAL_FIELD_ALWAYS_SERIALIZE_NOT_OPTIONAL = "ID0077"
ERROR_ID_VARIANT_COMPARISON = "ID0078"
ERROR_ID_VARIANT_DUPLICATE_TYPES = "ID0080"
ERROR_ID_VARIANT_STRUCTS = "ID0081"
ERROR_ID_NO_VARIANT_ENUM = "ID0082"
ERROR_ID_COMMAND_DUPLICATES_NAME_AND_ALIAS = "ID0083"
ERROR_ID_UNKOWN_ENUM_VALUE = "ID0084"
ERROR_ID_EITHER_CHECK_OR_PRIVILEGE = "ID0085"
ERROR_ID_DUPLICATE_ACTION_TYPE = "ID0086"
ERROR_ID_DUPLICATE_ACCESS_CHECK = "ID0087"
ERROR_ID_DUPLICATE_PRIVILEGE = "ID0088"
ERROR_ID_EMPTY_ACCESS_CHECK = "ID0089"
ERROR_ID_MISSING_ACCESS_CHECK = "ID0090"
ERROR_ID_STABILITY_UNKNOWN_VALUE = "ID0091"
ERROR_ID_DUPLICATE_UNSTABLE_STABILITY = "ID0092"


class IDLError(Exception):
    """Base class for all IDL exceptions."""

    pass


class ParserError(common.SourceLocation):
    """
    ParserError represents an error from the IDL compiler.

    A Parser error consists of
    - error_id - IDxxxx where xxxx is a 0 leading number.
    - msg - a string describing an error.
    - file_name - an IDL file which contained the EOFError.
    - line - the line number of the error or near enough.
    - column - the column number of the error or near enough.
    """

    def __init__(self, error_id, msg, file_name, line, column):
        # type: (str, str, str, int, int) -> None
        """Construct a parser error with source location information."""
        self.error_id = error_id
        self.msg = msg
        super(ParserError, self).__init__(file_name, line, column)

    def __str__(self):
        # type: () -> str
        """Return a formatted error.

        Example error message:
        test.idl: (17, 4): ID0008: Unknown IDL node 'cpp_namespac' for YAML entity 'global'.
        """
        msg = "%s: (%d, %d): %s: %s" % (os.path.basename(self.file_name), self.line, self.column,
                                        self.error_id, self.msg)
        return msg  # type: ignore


class ParserErrorCollection(object):
    """Collection of parser errors with source context information."""

    def __init__(self):
        # type: () -> None
        """Initialize ParserErrorCollection."""
        self._errors = []  # type: List[ParserError]

    def add(self, location, error_id, msg):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error message with file (line, column) information."""
        self._errors.append(
            ParserError(error_id, msg, location.file_name, location.line, location.column))

    def has_errors(self):
        # type: () -> bool
        """Have any errors been added to the collection?."""
        return len(self._errors) > 0

    def contains(self, error_id):
        # type: (str) -> bool
        """Check if the error collection has at least one message of a given error_id."""
        return len([a for a in self._errors if a.error_id == error_id]) > 0

    def to_list(self):
        # type: () -> List[str]
        """Return a list of formatted error messages."""
        return [str(error) for error in self._errors]

    def dump_errors(self):
        # type: () -> None
        """Print the list of errors."""
        print("Errors found while compiling IDL")
        for error_msg in self.to_list():
            print("%s\n\n" % error_msg)
        print("Found %s errors" % (len(self.to_list())))

    def count(self):
        # type: () -> int
        """Return the count of errors."""
        return len(self._errors)

    def __str__(self):
        # type: () -> str
        """Return a list of errors."""
        return ', '.join(self.to_list())  # type: ignore


class ParserContext(object):
    """
    IDL parser current file and error context.

    Responsible for:
    - keeping track of current file while parsing imported documents.
    - single class responsible for producing actual error messages.
    """

    def __init__(self, file_name, errors):
        # type: (str, ParserErrorCollection) -> None
        """Construct a new ParserContext."""
        self.errors = errors
        self.file_name = file_name

    def _add_error(self, location, error_id, msg):
        # type: (common.SourceLocation, str, str) -> None
        """
        Add an error with a source location information.

        This is usually directly from an idl.syntax or idl.ast class.
        """
        self.errors.add(location, error_id, msg)

    def _add_node_error(self, node, error_id, msg):
        # type: (yaml.nodes.Node, str, str) -> None
        """Add an error with source location information based on a YAML node."""
        self.errors.add(
            common.SourceLocation(self.file_name, node.start_mark.line, node.start_mark.column),
            error_id, msg)

    def add_unknown_root_node_error(self, node):
        # type: (yaml.nodes.Node) -> None
        """Add an error about an unknown YAML root node."""
        self._add_node_error(
            node, ERROR_ID_UNKNOWN_ROOT,
            ("Unrecognized IDL specification root level node '%s', only " +
             " (global, import, types, commands, and structs) are accepted") % (node.value))

    def add_unknown_node_error(self, node, name):
        # type: (yaml.nodes.Node, str) -> None
        """Add an error about an unknown node."""
        self._add_node_error(node, ERROR_ID_UNKNOWN_NODE,
                             "Unknown IDL node '%s' for YAML entity '%s'" % (node.value, name))

    def add_duplicate_symbol_error(self, location, name, duplicate_class_name, original_class_name):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about a duplicate symbol."""
        self._add_error(
            location, ERROR_ID_DUPLICATE_SYMBOL, "%s '%s' is a duplicate symbol of an existing %s" %
            (duplicate_class_name, name, original_class_name))

    def add_unknown_type_error(self, location, field_name, type_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about an unknown type."""
        self._add_error(location, ERROR_ID_UNKNOWN_TYPE,
                        "'%s' is an unknown type for field '%s'" % (type_name, field_name))

    def _is_node_type(self, node, node_name, expected_node_type):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode], str, str) -> bool
        """Return True if the yaml node type is expected, otherwise returns False and logs an error."""
        if not node.id == expected_node_type:
            self._add_node_error(
                node, ERROR_ID_IS_NODE_TYPE,
                "Illegal YAML node type '%s' for '%s', expected YAML node type '%s'" %
                (node.id, node_name, expected_node_type))
            return False
        return True

    def is_mapping_node(self, node, node_name):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode], str) -> bool
        """Return True if this YAML node is a Map."""
        return self._is_node_type(node, node_name, "mapping")

    def is_scalar_node(self, node, node_name):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode], str) -> bool
        """Return True if this YAML node is a Scalar."""
        return self._is_node_type(node, node_name, "scalar")

    def is_scalar_sequence(self, node, node_name):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode], str) -> bool
        """Return True if this YAML node is a Sequence of Scalars."""
        if self._is_node_type(node, node_name, "sequence"):
            for seq_node in node.value:
                if not self.is_scalar_node(seq_node, node_name):
                    return False
            return True
        return False

    def is_sequence_mapping(self, node, node_name):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode], str) -> bool
        """Return True if this YAML node is a Sequence of Mappings."""
        if self._is_node_type(node, node_name, "sequence"):
            for seq_node in node.value:
                if not self.is_mapping_node(seq_node, node_name):
                    return False
            return True
        return False

    def is_scalar_sequence_or_scalar_node(self, node, node_name):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode], str) -> bool
        """Return True if the YAML node is a Scalar or Sequence."""
        if not node.id == "scalar" and not node.id == "sequence":
            self._add_node_error(
                node, ERROR_ID_IS_NODE_TYPE_SCALAR_OR_SEQUENCE,
                "Illegal node type '%s' for '%s', expected either node type 'scalar' or 'sequence'"
                % (node.id, node_name))
            return False

        if node.id == "sequence":
            return self.is_scalar_sequence(node, node_name)

        return True

    def is_scalar_or_mapping_node(self, node, node_name):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode], str) -> bool
        """Return True if the YAML node is a Scalar or Mapping."""
        if not node.id == "scalar" and not node.id == "mapping":
            self._add_node_error(
                node, ERROR_ID_IS_NODE_TYPE_SCALAR_OR_MAPPING,
                "Illegal node type '%s' for '%s', expected either node type 'scalar' or 'mapping'" %
                (node.id, node_name))
            return False

        return True

    def is_scalar_bool_node(self, node, node_name):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode], str) -> bool
        """Return True if this YAML node is a Scalar and a valid boolean."""
        if not self._is_node_type(node, node_name, "scalar"):
            return False

        if not (node.value == "true" or node.value == "false"):
            self._add_node_error(
                node, ERROR_ID_IS_NODE_VALID_BOOL,
                "Illegal bool value for '%s', expected either 'true' or 'false'." % node_name)
            return False

        return True

    def get_bool(self, node):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> bool
        """Convert a scalar to a bool."""
        assert self.is_scalar_bool_node(node, "unknown")

        if node.value == "true":
            return True
        return False

    def get_list(self, node):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> List[str]
        """Get a YAML scalar or sequence node as a list of strings."""
        assert self.is_scalar_sequence_or_scalar_node(node, "unknown")
        if node.id == "scalar":
            return [node.value]
        # Unzip the list of ScalarNode
        return [v.value for v in node.value]

    def add_duplicate_error(self, node, node_name):
        # type: (yaml.nodes.Node, str) -> None
        """Add an error about a duplicate node."""
        self._add_node_error(node, ERROR_ID_DUPLICATE_NODE,
                             "Duplicate node found for '%s'" % (node_name))

    def add_missing_required_field_error(self, node, node_parent, node_name):
        # type: (yaml.nodes.Node, str, str) -> None
        """Add an error about a YAML node missing a required child."""
        self._add_node_error(
            node, ERROR_ID_MISSING_REQUIRED_FIELD,
            "IDL node '%s' is missing required scalar '%s'" % (node_parent, node_name))

    def add_missing_ast_required_field_error(self, location, ast_type, ast_parent, ast_name):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about a AST node missing a required child."""
        self._add_error(
            location, ERROR_ID_MISSING_AST_REQUIRED_FIELD,
            "%s '%s' is missing required scalar '%s'" % (ast_type, ast_parent, ast_name))

    def add_array_not_valid_error(self, location, ast_type, name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about a 'array' not being a valid type name."""
        self._add_error(location, ERROR_ID_ARRAY_NOT_VALID_TYPE,
                        "The %s '%s' cannot be named 'array'" % (ast_type, name))

    def add_bad_bson_type_error(self, location, ast_type, ast_parent, bson_type_name):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about a bad bson type."""
        self._add_error(
            location, ERROR_ID_BAD_BSON_TYPE, "BSON Type '%s' is not recognized for %s '%s'." %
            (bson_type_name, ast_type, ast_parent))

    def add_bad_bson_scalar_type_error(self, location, ast_type, ast_parent, bson_type_name):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about a bad list of bson types."""
        self._add_error(location, ERROR_ID_BAD_BSON_TYPE_LIST,
                        ("BSON Type '%s' is not a scalar bson type for %s '%s'" +
                         " and cannot be used in a list of bson serialization types.") %
                        (bson_type_name, ast_type, ast_parent))

    def add_bad_bson_bindata_subtype_error(self, location, ast_type, ast_parent, bson_type_name):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about a bindata_subtype associated with a type that is not bindata."""
        self._add_error(location, ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_TYPE,
                        ("The bindata_subtype field for %s '%s' is not valid for bson type '%s'") %
                        (ast_type, ast_parent, bson_type_name))

    def add_bad_bson_bindata_subtype_value_error(self, location, ast_type, ast_parent, value):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about a bad value for bindata_subtype."""
        self._add_error(location, ERROR_ID_BAD_BSON_BINDATA_SUBTYPE_VALUE,
                        ("The bindata_subtype field's value '%s' for %s '%s' is not valid") %
                        (value, ast_type, ast_parent))

    def add_bad_setat_specifier(self, location, specifier):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a bad set_at specifier."""
        self._add_error(
            location, ERROR_ID_BAD_SETAT_SPECIFIER,
            ("Unexpected set_at specifier: '%s', expected 'startup' or 'runtime'") % (specifier))

    def add_no_string_data_error(self, location, ast_type, ast_parent):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about using StringData for cpp_type."""
        self._add_error(location, ERROR_ID_NO_STRINGDATA,
                        ("Do not use mongo::StringData for %s '%s', use std::string instead") %
                        (ast_type, ast_parent))

    def add_ignored_field_must_be_empty_error(self, location, name, field_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about field must be empty for ignored fields."""
        self._add_error(
            location, ERROR_ID_FIELD_MUST_BE_EMPTY_FOR_IGNORED,
            ("Field '%s' cannot contain a value for property '%s' when a field is marked as ignored"
             ) % (name, field_name))

    def add_struct_default_must_be_true_or_empty_error(self, location, name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about default must be True or empty for fields of type struct."""
        self._add_error(location, ERROR_ID_DEFAULT_MUST_BE_TRUE_OR_EMPTY_FOR_STRUCT, (
            "Field '%s' can only contain value 'true' for property 'default' when a field's type is a struct"
        ) % (name))

    def add_not_custom_scalar_serialization_not_supported_error(  # pylint: disable=invalid-name
            self, location, ast_type, ast_parent, bson_type_name):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about field must be empty for fields of type struct."""
        self._add_error(
            location, ERROR_ID_CUSTOM_SCALAR_SERIALIZATION_NOT_SUPPORTED,
            ("Custom serialization for a scalar is only supported for 'string'. The %s '%s' cannot"
             + " use bson type '%s', use a bson_serialization_type of 'any' instead.") %
            (ast_type, ast_parent, bson_type_name))

    def add_bad_any_type_use_error(self, location, bson_type, ast_type, ast_parent):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about any being used in a list of bson types."""
        self._add_error(
            location, ERROR_ID_BAD_ANY_TYPE_USE,
            ("The BSON Type '%s' is not allowed in a list of bson serialization types for" +
             "%s '%s'. It must be only a single bson type.") % (bson_type, ast_type, ast_parent))

    def add_bad_cpp_numeric_type_use_error(self, location, ast_type, ast_parent, cpp_type):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about any being used in a list of bson types."""
        self._add_error(
            location, ERROR_ID_BAD_NUMERIC_CPP_TYPE,
            ("The C++ numeric type '%s' is not allowed for %s '%s'. Only 'std::int32_t'," +
             " 'std::uint32_t', 'std::uint64_t', and 'std::int64_t' are supported.") %
            (cpp_type, ast_type, ast_parent))

    def add_bad_array_type_name_error(self, location, field_name, type_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about a field type having a malformed type name."""
        self._add_error(location, ERROR_ID_BAD_ARRAY_TYPE_NAME,
                        ("'%s' is not a valid array type for field '%s'. A valid array type" +
                         " is in the form 'array<type_name>'.") % (type_name, field_name))

    def add_array_no_default_error(self, location, field_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about an array having a type with a default value."""
        self._add_error(
            location, ERROR_ID_ARRAY_NO_DEFAULT,
            "Field '%s' is not allowed to have both a default value and be an array type" %
            (field_name))

    def add_cannot_find_import(self, location, imported_file_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about not being able to find an import."""
        self._add_error(location, ERROR_ID_BAD_IMPORT,
                        "Could not resolve import '%s', file not found" % (imported_file_name))

    def add_bindata_no_default(self, location, ast_type, ast_parent):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about a bindata type with a default value."""
        self._add_error(location, ERROR_ID_BAD_BINDATA_DEFAULT,
                        ("Default values are not allowed for %s '%s'") % (ast_type, ast_parent))

    def add_chained_type_not_found_error(self, location, type_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a chained_type not found."""
        self._add_error(location, ERROR_ID_CHAINED_TYPE_NOT_FOUND,
                        ("Type '%s' is not a valid chained type") % (type_name))

    def add_chained_type_wrong_type_error(self, location, type_name, bson_type_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about a chained_type being the wrong type."""
        self._add_error(location, ERROR_ID_CHAINED_TYPE_WRONG_BSON_TYPE,
                        ("Chained Type '%s' has the wrong bson serialization type '%s', only" +
                         "'chain' is supported for chained types.") % (type_name, bson_type_name))

    def add_duplicate_field_error(self, location, field_container, field_name, duplicate_location):
        # type: (common.SourceLocation, str, str, common.SourceLocation) -> None
        """Add an error about duplicate fields as a result of chained structs/types."""
        self._add_error(
            location, ERROR_ID_CHAINED_DUPLICATE_FIELD,
            ("Chained Struct or Type '%s' duplicates an existing field '%s' at location" + "'%s'.")
            % (field_container, field_name, duplicate_location))

    def add_chained_type_no_strict_error(self, location, struct_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about strict parser validate and chained types."""
        self._add_error(location, ERROR_ID_CHAINED_NO_TYPE_STRICT,
                        ("Strict IDL parser validation is not supported with chained types for " +
                         "struct '%s'. Specify 'strict: false' for this struct.") % (struct_name))

    def add_chained_struct_not_found_error(self, location, struct_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a chained_struct not found."""
        self._add_error(location, ERROR_ID_CHAINED_STRUCT_NOT_FOUND,
                        ("Type '%s' is not a valid chained struct") % (struct_name))

    def add_chained_nested_struct_no_strict_error(self, location, struct_name, nested_struct_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about strict parser validate and chained types."""
        self._add_error(location, ERROR_ID_CHAINED_NO_NESTED_STRUCT_STRICT,
                        ("Strict IDL parser validation is not supported for a chained struct '%s'" +
                         " contained by struct '%s'. Specify 'strict: false' for this struct.") %
                        (nested_struct_name, struct_name))

    def add_chained_nested_struct_no_nested_error(self, location, struct_name, chained_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about struct's chaining being a struct with nested chaining."""
        self._add_error(location, ERROR_ID_CHAINED_NO_NESTED_CHAINED,
                        ("Struct '%s' is not allowed to nest struct '%s' since it has chained" +
                         " structs and/or types.") % (struct_name, chained_name))

    def add_empty_enum_error(self, node, name):
        # type: (yaml.nodes.Node, str) -> None
        """Add an error about an enum without values."""
        self._add_node_error(
            node, ERROR_ID_BAD_EMPTY_ENUM,
            "Enum '%s' must have values specified but no values were found" % (name))

    def add_array_enum_error(self, location, field_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error for a field being an array of enums."""
        self._add_error(location, ERROR_ID_NO_ARRAY_ENUM,
                        "Field '%s' cannot be an array of enums" % (field_name))

    def add_enum_bad_type_error(self, location, enum_name, enum_type):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error for an enum having the wrong type."""
        self._add_error(location, ERROR_ID_ENUM_BAD_TYPE,
                        "Enum '%s' type '%s' is not a supported enum type" % (enum_name, enum_type))

    def add_enum_value_not_int_error(self, location, enum_name, enum_value, err_msg):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error for an enum value not being an integer."""
        self._add_error(
            location, ERROR_ID_ENUM_BAD_INT_VAUE,
            "Enum '%s' value '%s' is not an integer, exception '%s'" % (enum_name, enum_value,
                                                                        err_msg))

    def add_enum_value_not_unique_error(self, location, enum_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error for an enum having duplicate values."""
        self._add_error(location, ERROR_ID_ENUM_NON_UNIQUE_VALUES,
                        "Enum '%s' has duplicate values, all values must be unique" % (enum_name))

    def add_enum_non_continuous_range_error(self, location, enum_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error for an enum having duplicate values."""
        self._add_error(location, ERROR_ID_ENUM_NON_CONTINUOUS_RANGE,
                        ("Enum '%s' has non-continuous integer variables, enums must have a " +
                         "continuous range of integer variables.") % (enum_name))

    def add_bad_command_namespace_error(self, location, command_name, command_namespace,
                                        valid_commands):
        # type: (common.SourceLocation, str, str, List[str]) -> None
        """Add an error about the namespace value not being a valid choice."""
        self._add_error(
            location, ERROR_ID_BAD_COMMAND_NAMESPACE,
            "Command namespace '%s' for command '%s' is not a valid choice. Valid options are '%s'."
            % (command_namespace, command_name, valid_commands))

    def add_bad_command_as_field_error(self, location, command_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about using a command for a field."""
        self._add_error(location, ERROR_ID_FIELD_NO_COMMAND,
                        ("Command '%s' cannot be used as a field type'. Commands must be top-level"
                         + " types due to their serialization rules.") % (command_name))

    def add_bad_array_of_chain(self, location, field_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a field being an array of chain_types."""
        self._add_error(location, ERROR_ID_NO_ARRAY_OF_CHAIN,
                        "Field '%s' cannot be an array of chained types" % (field_name))

    def add_bad_field_default_and_optional(self, location, field_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a field being optional and having a default value."""
        self._add_error(
            location, ERROR_ID_ILLEGAL_FIELD_DEFAULT_AND_OPTIONAL,
            ("Field '%s' can only be marked as optional or have a default value," + " not both.") %
            (field_name))

    def add_bad_struct_field_as_doc_sequence_error(self, location, struct_name, field_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about using a field in a struct being marked with supports_doc_sequence."""
        self._add_error(location, ERROR_ID_STRUCT_NO_DOC_SEQUENCE,
                        ("Field '%s' in struct '%s' cannot be used as a Command Document Sequence"
                         " type. They are only supported in commands.") % (field_name, struct_name))

    def add_bad_non_array_as_doc_sequence_error(self, location, struct_name, field_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about using a non-array type field being marked with supports_doc_sequence."""
        self._add_error(location, ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_ARRAY,
                        ("Field '%s' in command '%s' cannot be used as a Command Document Sequence"
                         " type since it is not an array.") % (field_name, struct_name))

    def add_bad_non_object_as_doc_sequence_error(self, location, field_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about using a non-struct or BSON object for a doc sequence."""
        self._add_error(location, ERROR_ID_NO_DOC_SEQUENCE_FOR_NON_OBJECT,
                        ("Field '%s' cannot be used as a Command Document Sequence"
                         " type since it is not a BSON object or struct.") % (field_name))

    def add_bad_command_name_duplicates_field(self, location, command_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a command and field having the same name."""
        self._add_error(location, ERROR_ID_COMMAND_DUPLICATES_FIELD,
                        ("Command '%s' cannot have the same name as a field.") % (command_name))

    def add_bad_field_non_const_getter_in_immutable_struct_error(  # pylint: disable=invalid-name
            self, location, struct_name, field_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about marking a field with non_const_getter in an immutable struct."""
        self._add_error(
            location, ERROR_ID_NON_CONST_GETTER_IN_IMMUTABLE_STRUCT,
            ("Cannot generate a non-const getter for field '%s' in struct '%s' since"
             " struct '%s' is marked as immutable.") % (field_name, struct_name, struct_name))

    def add_useless_variant_error(self, location):
        # type: (common.SourceLocation) -> None
        """Add an error about a variant with 0 or 1 variant types."""
        self._add_error(location, ERROR_ID_USELESS_VARIANT,
                        ("Cannot declare a variant with only 0 or 1 variant types"))

    def add_variant_comparison_error(self, location):
        # type: (common.SourceLocation) -> None
        """Add an error about a struct with generate_comparison_operators and a variant field."""
        self._add_error(location, ERROR_ID_VARIANT_COMPARISON,
                        ("generate_comparison_operators is not supported with variant types"))

    def add_variant_duplicate_types_error(self, location, field_name, type_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about a variant having more than one alternative of the same BSON type."""
        self._add_error(
            location, ERROR_ID_VARIANT_DUPLICATE_TYPES,
            ("Variant field '%s' has multiple alternatives with BSON type '%s', this is prohibited"
             " to avoid ambiguity while parsing BSON.") % (field_name, type_name))

    def add_variant_structs_error(self, location, field_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a variant having more than one struct alternative."""
        self._add_error(location, ERROR_ID_VARIANT_STRUCTS,
                        ("Variant field '%s' has multiple struct alternatives, this is prohibited"
                         " to avoid ambiguity while parsing BSON subdocuments.") % (field_name, ))

    def add_variant_enum_error(self, location, field_name, type_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error for a variant that can be an enum."""
        self._add_error(
            location, ERROR_ID_NO_VARIANT_ENUM,
            "Field '%s' cannot be a variant with an enum alternative type '%s'" % (field_name,
                                                                                   type_name))

    def is_scalar_non_negative_int_node(self, node, node_name):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode], str) -> bool
        """Return True if this YAML node is a Scalar and a valid non-negative int."""
        if not self._is_node_type(node, node_name, "scalar"):
            return False

        try:
            value = int(node.value)
            if value < 0:
                self._add_node_error(
                    node, ERROR_ID_IS_NODE_VALID_NON_NEGATIVE_INT,
                    "Illegal negative integer value for '%s', expected 0 or positive integer." %
                    (node_name))
                return False

        except ValueError as value_error:
            self._add_node_error(
                node, ERROR_ID_IS_NODE_VALID_INT,
                "Illegal integer value for '%s', message '%s'." % (node_name, value_error))
            return False

        return True

    def get_non_negative_int(self, node):
        # type: (Union[yaml.nodes.MappingNode, yaml.nodes.ScalarNode, yaml.nodes.SequenceNode]) -> int
        """Convert a scalar to an int."""
        assert self.is_scalar_non_negative_int_node(node, "unknown")

        return int(node.value)

    def add_duplicate_comparison_order_field_error(self, location, struct_name, comparison_order):
        # type: (common.SourceLocation, str, int) -> None
        """Add an error about fields having duplicate comparison_orders."""
        self._add_error(
            location, ERROR_ID_IS_DUPLICATE_COMPARISON_ORDER,
            ("Struct '%s' cannot have two fields with the same comparison_order value '%d'.") %
            (struct_name, comparison_order))

    def add_extranous_command_type(self, location, command_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about commands having type when not needed."""
        self._add_error(
            location, ERROR_ID_IS_COMMAND_TYPE_EXTRANEOUS,
            ("Command '%s' cannot have a 'type' property unless namespace equals 'type'.") %
            (command_name))

    def add_value_not_numeric_error(self, location, attrname, value):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about non-numeric value where number expected."""
        self._add_error(
            location, ERROR_ID_VALUE_NOT_NUMERIC,
            ("'%s' requires a numeric value, but %s can not be cast") % (attrname, value))

    def add_server_parameter_invalid_attr(self, location, attrname, conflicts):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about invalid fields in a server parameter definition."""
        self._add_error(
            location, ERROR_ID_SERVER_PARAMETER_INVALID_ATTR,
            ("'%s' attribute not permitted with '%s' server parameter") % (attrname, conflicts))

    def add_server_parameter_required_attr(self, location, attrname, required, dependant=None):
        # type: (common.SourceLocation, str, str, str) -> None
        """Add an error about missing fields in a server parameter definition."""
        qualifier = '' if dependant is None else (" when using '%s' attribute" % (dependant))
        self._add_error(location, ERROR_ID_SERVER_PARAMETER_REQUIRED_ATTR,
                        ("'%s' attribute required%s with '%s' server parameter") %
                        (attrname, qualifier, required))

    def add_server_parameter_invalid_method_override(self, location, method):
        # type: (common.SourceLocation, str) -> None
        """Add an error about invalid method override in SCP method override."""
        self._add_error(location, ERROR_ID_SERVER_PARAMETER_INVALID_METHOD_OVERRIDE,
                        ("No such method to override in server parameter class: '%s'") % (method))

    def add_bad_source_specifier(self, location, value):
        # type: (common.SourceLocation, str) -> None
        """Add an error about invalid source specifier."""
        self._add_error(location, ERROR_ID_BAD_SOURCE_SPECIFIER,
                        ("'%s' is not a valid source specifier") % (value))

    def add_bad_duplicate_behavior(self, location, value):
        # type: (common.SourceLocation, str) -> None
        """Add an error about invalid duplicate behavior specifier."""
        self._add_error(location, ERROR_ID_BAD_DUPLICATE_BEHAVIOR_SPECIFIER,
                        ("'%s' is not a valid duplicate behavior specifier") % (value))

    def add_bad_numeric_range(self, location, attrname, value):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about invalid range specifier."""
        self._add_error(location, ERROR_ID_BAD_NUMERIC_RANGE,
                        ("'%s' is not a valid numeric range for '%s'") % (value, attrname))

    def add_missing_shortname_for_positional_arg(self, location):
        # type: (common.SourceLocation) -> None
        """Add an error about required short_name for positional args."""
        self._add_error(location, ERROR_ID_MISSING_SHORTNAME_FOR_POSITIONAL,
                        "Missing 'short_name' for positional arg")

    def add_invalid_short_name(self, location, name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about invalid short names."""
        self._add_error(location, ERROR_ID_INVALID_SHORT_NAME,
                        ("Invalid 'short_name' value '%s'") % (name))

    def add_invalid_single_name(self, location, name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about invalid single names."""
        self._add_error(location, ERROR_ID_INVALID_SINGLE_NAME,
                        ("Invalid 'single_name' value '%s'") % (name))

    def add_missing_short_name_with_single_name(self, location, name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about missing required short name when using single name."""
        self._add_error(location, ERROR_ID_MISSING_SHORT_NAME_WITH_SINGLE_NAME,
                        ("Missing 'short_name' required with 'single_name' value '%s'") % (name))

    def add_feature_flag_default_true_missing_version(self, location):
        # type: (common.SourceLocation) -> None
        """Add an error about a default flag with a default value of true but no version."""
        self._add_error(location, ERROR_ID_FEATURE_FLAG_DEFAULT_TRUE_MISSING_VERSION,
                        ("Missing 'version' required for feature flag that defaults to true"))

    def add_feature_flag_default_false_has_version(self, location):
        # type: (common.SourceLocation) -> None
        """Add an error about a default flag with a default value of false but has a version."""
        self._add_error(
            location, ERROR_ID_FEATURE_FLAG_DEFAULT_FALSE_HAS_VERSION,
            ("The 'version' attribute is not allowed for feature flag that defaults to false"))

    def add_reply_type_invalid_type(self, location, command_name, reply_type_name):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about a command whose reply_type refers to an unknown type."""
        self._add_error(
            location, ERROR_ID_INVALID_REPLY_TYPE,
            ("Command '%s' has invalid reply_type '%s'" % (command_name, reply_type_name)))

    def add_stability_no_api_version(self, location, command_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a command with 'stability' but no 'api_version'."""
        self._add_error(
            location, ERROR_ID_STABILITY_NO_API_VERSION,
            ("Command '%s' specifies 'stability' but has no 'api_version'" % (command_name, )))

    def add_missing_reply_type(self, location, command_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a command with 'api_version' but no 'reply_type'."""
        self._add_error(
            location, ERROR_ID_MISSING_REPLY_TYPE,
            ("Command '%s' has an 'api_version' but no 'reply_type'" % (command_name, )))

    def add_bad_field_always_serialize_not_optional(self, location, field_name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a field with 'always_serialize' but 'optional' isn't set to true."""
        self._add_error(
            location, ERROR_ID_ILLEGAL_FIELD_ALWAYS_SERIALIZE_NOT_OPTIONAL,
            ("Field '%s' specifies 'always_serialize' but 'optional' isn't true.") % (field_name))

    def add_duplicate_command_name_and_alias(self, node):
        # type: (yaml.nodes.Node) -> None
        """Add an error about a command name and command alias having the same name."""
        self._add_node_error(node, ERROR_ID_COMMAND_DUPLICATES_NAME_AND_ALIAS,
                             "Duplicate command_name and command_alias found.")

    def add_unknown_enum_value(self, location, enum_name, enum_value):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about an unknown enum value."""
        self._add_error(location, ERROR_ID_UNKOWN_ENUM_VALUE,
                        "Cannot find enum value '%s' in enum '%s'." % (enum_value, enum_name))

    def add_either_check_or_privilege(self, location):
        # type: (common.SourceLocation) -> None
        """Add an error about specifing both a check and a privilege or neither."""
        self._add_error(
            location, ERROR_ID_EITHER_CHECK_OR_PRIVILEGE,
            "Must specify either a 'check' and a 'privilege' in an access_check, not both.")

    def add_duplicate_action_types(self, location, name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about specifying an action type twice in the same list."""
        self._add_error(location, ERROR_ID_DUPLICATE_ACTION_TYPE,
                        "Cannot specify an action_type '%s' more then once" % (name))

    def add_duplicate_access_check(self, location, name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about specifying an access check twice in the same list."""
        self._add_error(location, ERROR_ID_DUPLICATE_ACCESS_CHECK,
                        "Cannot specify an access_check '%s' more then once" % (name))

    def add_duplicate_privilege(self, location, resource_pattern, action_type):
        # type: (common.SourceLocation, str, str) -> None
        """Add an error about specifying a privilege twice in the same list."""
        self._add_error(
            location, ERROR_ID_DUPLICATE_PRIVILEGE,
            "Cannot specify the pair of resource_pattern '%s' and action_type '%s' more then once" %
            (resource_pattern, action_type))

    def add_empty_access_check(self, location):
        # type: (common.SourceLocation) -> None
        """Add an error about specifying one of ignore, none, simple or complex in an access check."""
        self._add_error(
            location, ERROR_ID_EMPTY_ACCESS_CHECK,
            "Must one and only one of either a 'ignore', 'none', 'simple', or 'complex' in an access_check."
        )

    def add_missing_access_check(self, location, name):
        # type: (common.SourceLocation, str) -> None
        """Add an error about a missing access_check when api_version != ""."""
        self._add_error(location, ERROR_ID_MISSING_ACCESS_CHECK,
                        'Command "%s" has api_version != "" but is missing access_check.' % (name))

    def add_stability_unknown_value(self, location):
        # type: (common.SourceLocation) -> None
        """Add an error about a field with unknown value set to 'stability' option."""
        self._add_error(
            location, ERROR_ID_STABILITY_UNKNOWN_VALUE,
            "Field option 'stability' has unknown value, should be one of 'stable', 'unstable' or 'internal.'"
        )

    def add_duplicate_unstable_stability(self, location):
        # type: (common.SourceLocation) -> None
        """Add an error about a field specifying both 'unstable' and 'stability'."""
        self._add_error(location, ERROR_ID_DUPLICATE_UNSTABLE_STABILITY, (
            "Field specifies both 'unstable' and 'stability' options, should use 'stability: [stable|unstable|internal]' instead and remove the deprecated 'unstable' option."
        ))


def _assert_unique_error_messages():
    # type: () -> None
    """Assert that error codes are unique."""
    error_ids = []
    for module_member in inspect.getmembers(sys.modules[__name__]):
        if module_member[0].startswith("ERROR_ID"):
            error_ids.append(module_member[1])

    error_ids_set = set(error_ids)
    if len(error_ids) != len(error_ids_set):
        raise IDLError("IDL error codes prefixed with ERROR_ID are not unique.")


# On file import, check the error messages are unique
_assert_unique_error_messages()
