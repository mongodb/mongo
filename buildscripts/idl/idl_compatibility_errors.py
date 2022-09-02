# Copyright (C) 2021-present MongoDB, Inc.
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
Common error handling code for IDL compatibility checker.

- Common Exceptions used by IDL compatibility checker.
- Error codes used by the IDL compatibility checker.
"""

import inspect
import os
import sys
from typing import List, Optional

# Public error codes used by IDL compatibility checker.
# Used by tests cases to validate expected errors are thrown in negative tests.
# Error codes must be unique, validated  _assert_unique_error_messages on file load.
#
ERROR_ID_COMMAND_INVALID_API_VERSION = "ID0001"
ERROR_ID_DUPLICATE_COMMAND_NAME = "ID0002"
ERROR_ID_REMOVED_COMMAND = "ID0003"
ERROR_ID_NEW_REPLY_FIELD_UNSTABLE = "ID0004"
ERROR_ID_NEW_REPLY_FIELD_OPTIONAL = "ID0005"
ERROR_ID_NEW_REPLY_FIELD_MISSING = "ID0006"
ERROR_ID_NEW_REPLY_FIELD_TYPE_NOT_STRUCT = "ID0007"
ERROR_ID_NEW_REPLY_FIELD_TYPE_NOT_ENUM = "ID0008"
ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY = "ID0009"
ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY = "ID0010"
ERROR_ID_NEW_REPLY_FIELD_TYPE_ENUM_OR_STRUCT = "ID0011"
ERROR_ID_REPLY_FIELD_TYPE_INVALID = "ID0012"
ERROR_ID_REPLY_FIELD_NOT_SUBSET = "ID0013"
ERROR_ID_NEW_NAMESPACE_INCOMPATIBLE = "ID0014"
ERROR_ID_COMMAND_TYPE_NOT_SUPERSET = "ID0015"
ERROR_ID_COMMAND_TYPE_INVALID = "ID0016"
ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY = "ID0017"
ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY = "ID0018"
ERROR_ID_NEW_COMMAND_TYPE_FIELD_MISSING = "ID0019"
ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRED = "ID0020"
ERROR_ID_NEW_COMMAND_TYPE_FIELD_UNSTABLE = "ID0021"
ERROR_ID_NEW_COMMAND_TYPE_NOT_STRUCT = "ID0022"
ERROR_ID_NEW_COMMAND_TYPE_NOT_ENUM = "ID0023"
ERROR_ID_NEW_COMMAND_TYPE_ENUM_OR_STRUCT = "ID0024"
ERROR_ID_MISSING_ERROR_REPLY_STRUCT = "ID0025"
ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE = "ID0026"
ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET = "ID0027"
ERROR_ID_REMOVED_COMMAND_PARAMETER = "ID0028"
ERROR_ID_ADDED_REQUIRED_COMMAND_PARAMETER = "ID0029"
ERROR_ID_COMMAND_PARAMETER_UNSTABLE = "ID0030"
ERROR_ID_COMMAND_PARAMETER_STABLE_REQUIRED_NO_DEFAULT = "ID0031"
ERROR_ID_COMMAND_PARAMETER_REQUIRED = "ID0032"
ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY = "ID0033"
ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY = "ID0034"
ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_NOT_STRUCT = "ID0035"
ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_NOT_ENUM = "ID0036"
ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_ENUM_OR_STRUCT = "ID0037"
ERROR_ID_COMMAND_PARAMETER_TYPE_INVALID = "ID0038"
ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET = "ID0039"
ERROR_ID_REPLY_FIELD_CONTAINS_VALIDATOR = "ID0040"
ERROR_ID_COMMAND_PARAMETER_CONTAINS_VALIDATOR = "ID0041"
ERROR_ID_COMMAND_PARAMETER_VALIDATORS_NOT_EQUAL = "ID0042"
ERROR_ID_COMMAND_TYPE_CONTAINS_VALIDATOR = "ID0043"
ERROR_ID_COMMAND_TYPE_VALIDATORS_NOT_EQUAL = "ID0044"
ERROR_ID_NEW_COMMAND_TYPE_FIELD_STABLE_REQUIRED_NO_DEFAULT = "ID0045"
ERROR_ID_NEW_COMMAND_TYPE_FIELD_ADDED_REQUIRED = "ID0046"
ERROR_ID_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED = "ID0047"
ERROR_ID_COMMAND_PARAMETER_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED = "ID0048"
ERROR_ID_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED = "ID0049"
ERROR_ID_COMMAND_PARAMETER_CPP_TYPE_NOT_EQUAL = "ID0050"
ERROR_ID_COMMAND_CPP_TYPE_NOT_EQUAL = "ID0051"
ERROR_ID_REPLY_FIELD_CPP_TYPE_NOT_EQUAL = "ID0052"
ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_NOT_VARIANT = "ID0053"
ERROR_ID_NEW_COMMAND_TYPE_NOT_VARIANT = "ID0054"
ERROR_ID_NEW_COMMAND_PARAMETER_VARIANT_TYPE_NOT_SUPERSET = "ID0055"
ERROR_ID_NEW_COMMAND_VARIANT_TYPE_NOT_SUPERSET = "ID0056"
ERROR_ID_REPLY_FIELD_VALIDATORS_NOT_EQUAL = "ID0057"
ERROR_ID_CHECK_NOT_EQUAL = "ID0058"
ERROR_ID_RESOURCE_PATTERN_NOT_EQUAL = "ID0059"
ERROR_ID_NEW_ACTION_TYPES_NOT_SUBSET = "ID0060"
ERROR_ID_TYPE_NOT_ARRAY = "ID0061"
ERROR_ID_ACCESS_CHECK_TYPE_NOT_EQUAL = "ID0062"
ERROR_ID_NEW_COMPLEX_CHECKS_NOT_SUBSET = "ID0063"
ERROR_ID_NEW_COMPLEX_PRIVILEGES_NOT_SUBSET = "ID0064"
ERROR_ID_NEW_ADDITIONAL_COMPLEX_ACCESS_CHECK = "ID0065"
ERROR_ID_REMOVED_ACCESS_CHECK_FIELD = "ID0066"
ERROR_ID_ADDED_ACCESS_CHECK_FIELD = "ID0067"
ERROR_ID_COMMAND_STRICT_TRUE_ERROR = "ID0068"
ERROR_ID_GENERIC_ARGUMENT_REMOVED = "ID0069"
ERROR_ID_GENERIC_ARGUMENT_REMOVED_REPLY_FIELD = "ID0070"
ERROR_ID_COMMAND_PARAMETER_SERIALIZER_NOT_EQUAL = "ID0071"
ERROR_ID_COMMAND_SERIALIZER_NOT_EQUAL = "ID0072"
ERROR_ID_REPLY_FIELD_SERIALIZER_NOT_EQUAL = "ID0073"
ERROR_ID_COMMAND_DESERIALIZER_NOT_EQUAL = "ID0074"
ERROR_ID_COMMAND_PARAMETER_DESERIALIZER_NOT_EQUAL = "ID0075"
ERROR_ID_REPLY_FIELD_DESERIALIZER_NOT_EQUAL = "ID0076"
ERROR_ID_NEW_REPLY_FIELD_REQUIRES_STABILITY = "ID0077"
ERROR_ID_NEW_PARAMETER_REQUIRES_STABILITY = "ID0078"
ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRES_STABILITY = "ID0079"
ERROR_ID_NEW_REPLY_CHAINED_TYPE_NOT_SUBSET = "ID0080"
ERROR_ID_NEW_COMMAND_PARAMETER_CHAINED_TYPE_NOT_SUPERSET = "ID0081"
ERROR_ID_NEW_COMMAND_CHAINED_TYPE_NOT_SUPERSET = "ID0082"
ERROR_ID_UNSTABLE_REPLY_FIELD_CHANGED_TO_STABLE = "ID0083"
ERROR_ID_UNSTABLE_COMMAND_PARAM_FIELD_CHANGED_TO_STABLE = "ID0084"
ERROR_ID_UNSTABLE_COMMAND_TYPE_FIELD_CHANGED_TO_STABLE = "ID0085"
ERROR_ID_NEW_REPLY_FIELD_ADDED_AS_STABLE = "ID0086"
ERROR_ID_NEW_COMMAND_PARAM_FIELD_ADDED_AS_STABLE = "ID0087"
ERROR_ID_NEW_COMMAND_TYPE_FIELD_ADDED_AS_STABLE = "ID0088"


class IDLCompatibilityCheckerError(Exception):
    """Base class for all IDL Compatibility Checker exceptions."""

    pass


class IDLCompatibilityError(object):
    """
    IDLCompatibilityError represents an error from the IDL compatibility checker.

    An IDLCompatibilityError consists of
    - error_id - IDxxxx where xxxx is a 0 leading number.
    - command_name - a string, the command where the error occurred.
    - msg - a string describing an error.
    - old_idl_dir - a string, the directory containing the old IDL files.
    - new_idl_dir - a string, the directory containing the new IDL files.
    - file - a string, the path to the IDL file where the error occurred.
    """

    def __init__(self, error_id: str, command_name: str, msg: str, old_idl_dir: str,
                 new_idl_dir: str, file: str) -> None:
        """Construct an IDLCompatibility error."""
        self.error_id = error_id
        self.command_name = command_name
        self.msg = msg
        self.old_idl_dir = old_idl_dir
        self.new_idl_dir = new_idl_dir
        self.file = file

    def __str__(self) -> str:
        """Return a formatted error.

        Example error message:
        Comparing compatibility_test_pass_old and compatibility_test_pass_new:
        Error in compatibility_test_pass_new/file.idl: ID0001: 'command' has an invalid API
        version '2'.
        """
        msg = "Comparing %s and %s: Error in %s: %s: %s" % (self.old_idl_dir, self.new_idl_dir,
                                                            self.file, self.error_id, self.msg)
        return msg


class IDLCompatibilityErrorCollection(object):
    """Collection of IDL compatibility errors with source context information."""

    def __init__(self) -> None:
        """Initialize IDLCompatibilityErrorCollection."""
        self._errors: List[IDLCompatibilityError] = []

    def add(self, error_id: str, command_name: str, msg: str, old_idl_dir: str, new_idl_dir: str,
            file: str) -> None:
        """Add an error message with directory information."""
        self._errors.append(
            IDLCompatibilityError(error_id, command_name, msg, old_idl_dir, new_idl_dir, file))

    def has_errors(self) -> bool:
        """Have any errors been added to the collection?."""
        return len(self._errors) > 0

    def contains(self, error_id: str) -> bool:
        """Check if the error collection has at least one message of a given error_id."""
        return len([a for a in self._errors if a.error_id == error_id]) > 0

    def get_error_by_error_id(self, error_id: str) -> IDLCompatibilityError:
        """Get the first error in the error collection with the id error_id."""
        error_id_list = [a for a in self._errors if a.error_id == error_id]
        error = next(iter(error_id_list), None)
        assert error is not None
        return error

    def get_error_by_command_name(self, command_name: str) -> IDLCompatibilityError:
        """Get the first error in the error collection with the command command_name."""
        command_name_list = [a for a in self._errors if a.command_name == command_name]
        error = next(iter(command_name_list), None)
        assert error is not None
        return error

    def get_error_by_command_name_and_error_id(self, command_name: str,
                                               error_id: str) -> IDLCompatibilityError:
        """Get the first error in the error collection from command_name with error_id."""
        command_name_list = [a for a in self._errors if a.command_name == command_name]
        error_id_list = [a for a in command_name_list if a.error_id == error_id]
        error = next(iter(error_id_list), None)
        assert error is not None
        return error

    def get_all_errors_by_command_name(self, command_name: str) -> List[IDLCompatibilityError]:
        """Get all the errors in the error collection with the command command_name."""
        return [a for a in self._errors if a.command_name == command_name]

    def to_list(self) -> List[str]:
        """Return a list of formatted error messages."""
        return [str(error) for error in self._errors]

    def dump_errors(self) -> None:
        """Print the list of errors."""
        error_list = self.to_list()
        print("Errors found while checking IDL compatibility: %s errors:" % (len(error_list)))
        for error_msg in error_list:
            print("%s\n\n" % error_msg)
        print("------------------------------------------------")

    def count(self) -> int:
        """Return the count of errors."""
        return len(self._errors)

    def __str__(self) -> str:
        """Return a list of errors."""
        return ', '.join(self.to_list())


class IDLCompatibilityContext(object):
    """
    IDL compatibility current file and error context.

    Responsible for:
    - keeping track of current file while parsing imported documents.
    - single class responsible for producing actual error messages.
    """

    def __init__(self, old_idl_dir: str, new_idl_dir: str,
                 errors: IDLCompatibilityErrorCollection) -> None:
        """Construct a new IDLCompatibilityContext."""
        self.old_idl_dir = old_idl_dir
        self.new_idl_dir = new_idl_dir
        self.errors = errors

    def _add_error(self, error_id: str, command_name: str, msg: str, file: str) -> None:
        """Add an error with an error id and error message."""
        self.errors.add(error_id, command_name, msg, self.old_idl_dir, self.new_idl_dir, file)

    def add_command_invalid_api_version_error(self, command_name: str, api_version: str,
                                              file: str) -> None:
        """Add an error about a command with an invalid api version."""
        self._add_error(ERROR_ID_COMMAND_INVALID_API_VERSION, command_name,
                        "'%s' has an invalid API version '%s'" % (command_name, api_version), file)

    def add_command_removed_error(self, command_name: str, file: str) -> None:
        """Add an error about a command that was removed."""
        self._add_error(
            ERROR_ID_REMOVED_COMMAND, command_name,
            "The command '%s' was present in the stable API but was removed." % (command_name),
            file)

    def add_command_strict_true_error(self, command_name: str, file: str) -> None:
        """Add an error about a command that changes from strict: false to strict: true."""
        self._add_error(
            ERROR_ID_COMMAND_STRICT_TRUE_ERROR, command_name,
            "'%s' changes from strict: false in the old definition to strict: true in the new definition."
            % (command_name), file)

    def add_duplicate_command_name_error(self, command_name: str, dir_name: str, file: str) -> None:
        """Add an error about a duplicate command name within a directory."""
        self._add_error(ERROR_ID_DUPLICATE_COMMAND_NAME, command_name,
                        "'%s' has duplicate command: '%s'" % (dir_name, command_name), file)

    def add_reply_field_not_subset_error(self, command_name: str, field_name: str, type_name: str,
                                         file: str) -> None:
        """Add an error about the reply field not being a subset."""
        self._add_error(
            ERROR_ID_REPLY_FIELD_NOT_SUBSET, command_name,
            "'%s' has a reply field or sub-field '%s' with type '%s' "
            "that is not a subset of the type of the older definition "
            "of this reply field." % (command_name, field_name, type_name), file)

    def add_command_or_param_type_invalid_error(self, command_name: str, file: str,
                                                field_name: Optional[str],
                                                is_command_parameter: bool) -> None:
        """Add an error about the command parameter or type being invalid."""
        if is_command_parameter:
            self._add_error(
                ERROR_ID_COMMAND_PARAMETER_TYPE_INVALID, command_name,
                "The '%s' command has a field or sub-field '%s' that has an invalid type" %
                (command_name, field_name), file)
        else:
            self._add_error(
                ERROR_ID_COMMAND_TYPE_INVALID, command_name,
                "'%s' has an invalid type or has a sub-struct with an invalid type" %
                (command_name), file)

    def add_command_or_param_type_not_superset_error(self, command_name: str, type_name: str,
                                                     file: str, field_name: Optional[str],
                                                     is_command_parameter: bool) -> None:
        """Add an error about the command or parameter type not being a superset."""
        if is_command_parameter:
            self._add_error(
                ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET, command_name,
                "The new definition of command '%s' has field or sub-field '%s' with type '%s' "
                "that is not a superset of the "
                "type of the existing definition of the field." % (command_name, field_name,
                                                                   type_name), file)
        else:
            self._add_error(
                ERROR_ID_COMMAND_TYPE_NOT_SUPERSET, command_name,
                "The new definition of command '%s' or its sub-struct has type '%s' that is not a "
                "superset of "
                "the type of the existing definition of this command/struct." % (command_name,
                                                                                 type_name), file)

    def add_command_or_param_type_contains_validator_error(self, command_name: str, field_name: str,
                                                           file: str, type_name: Optional[str],
                                                           is_command_parameter: bool) -> None:
        """
        Add an error about a type containing a validator.

        Add an error about the new command or parameter type containing a validator
        while the old command or parameter type does not.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_COMMAND_PARAMETER_CONTAINS_VALIDATOR, command_name,
                "The new definition of the field or sub-field '%s' for the command '%s' contains a validator "
                "while the old definition does not." % (field_name, command_name), file)
        else:
            self._add_error(
                ERROR_ID_COMMAND_TYPE_CONTAINS_VALIDATOR, command_name,
                "The new definition of the command '%s' or its sub-struct has type '%s' with field '%s' that "
                "contains a validator when "
                "the old definition did not." % (command_name, type_name, field_name), file)

    def add_command_or_param_type_validators_not_equal_error(
            self, command_name: str, field_name: str, file: str, type_name: Optional[str],
            is_command_parameter: bool) -> None:
        # pylint: disable=invalid-name
        """Add an error about the new and old command or parameter type validators not being equal."""
        if is_command_parameter:
            self._add_error(
                ERROR_ID_COMMAND_PARAMETER_VALIDATORS_NOT_EQUAL, command_name,
                "Validator for field or sub-field '%s' in old definition of command '%s' is not equal "
                "to the validator in the new definition of the field" % (field_name, command_name),
                file)
        else:
            self._add_error(
                ERROR_ID_COMMAND_TYPE_VALIDATORS_NOT_EQUAL, command_name,
                "Validator for field '%s' in type '%s' in old definition of command '%s' or its "
                "sub-struct is not equal to the validator in the new defition of the command/struct."
                % (field_name, type_name, command_name), file)

    def add_missing_error_reply_struct_error(self, file: str) -> None:
        """Add an error about the file missing the ErrorReply struct."""
        self._add_error(ERROR_ID_MISSING_ERROR_REPLY_STRUCT, "n/a",
                        ("'%s' is missing the ErrorReply struct") % (file), file)

    def add_new_command_or_param_type_bson_any_error(
            self, command_name: str, old_type: str, new_type: str, file: str,
            field_name: Optional[str], is_command_parameter: bool) -> None:
        """
        Add an error about BSON serialization type.

        Add an error about the new command or command parameter type's
        bson serialization type being of type 'any' when the old type is non-any or
        when it is not explicitly allowed.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY, command_name,
                "The new definition of '%s' command has field or sub-field '%s' that has type '%s' "
                "that has a bson serialization type 'any', while the existing older definition had type %s"
                " that did not have bson serialization type 'any'" % (command_name, field_name,
                                                                      new_type, old_type), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY, command_name,
                "The new definition of '%s' command or its sub-struct has type '%s' that "
                "has a bson serialization type 'any', while the existing older definition had type %s"
                " that did not have bson serialization type 'any'" % (command_name, new_type,
                                                                      old_type), file)

    def add_new_command_or_param_type_enum_or_struct_error(
            self, command_name: str, new_type: str, old_type: str, file: str,
            field_name: Optional[str], is_command_parameter: bool) -> None:
        """
        Add an error about a type that is an enum or struct.

        Add an error when the new command or command parameter type is an enum or
        struct and the old one is a type that is not an enum or struct.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_ENUM_OR_STRUCT, command_name,
                "The command '%s' has field or sub-field '%s' of type '%s' that is an enum or "
                "struct while the old definition of the field type is a non-enum or "
                "non-struct of type '%s'." % (command_name, field_name, new_type, old_type), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_ENUM_OR_STRUCT, command_name,
                "The command '%s' or its sub-struct has type '%s' that is an enum "
                "or struct while the old definition of the"
                "type was a non-enum or struct of type '%s'." % (command_name, new_type, old_type),
                file)

    def add_new_param_or_command_type_field_added_required_error(
            self, command_name: str, field_name: str, file: str, type_name: str,
            is_command_parameter: bool) -> None:
        # pylint: disable=invalid-name
        """
        Add a new added required parameter or command type field error.

        Add an error about an added required command parameter or command type field that did not
        exist in the old command.
        The added parameter or command type field should be optional.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_ADDED_REQUIRED_COMMAND_PARAMETER, command_name,
                "New definition of field or sub-field '%s' for command '%s' is required when it should "
                "be optional." % (field_name, command_name), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_FIELD_ADDED_REQUIRED, command_name,
                "The new definition of command '%s' or its sub-struct has type '%s' with an added and "
                "required type field '%s' that did not exist "
                "in the old definition of struct type." % (command_name, type_name, field_name),
                file)

    def add_new_param_or_command_type_field_missing_error(self, command_name: str, field_name: str,
                                                          file: str, type_name: str,
                                                          is_command_parameter: bool) -> None:
        """Add an error about a parameter or command type field that is missing in the new command."""
        if is_command_parameter:
            self._add_error(
                ERROR_ID_REMOVED_COMMAND_PARAMETER, command_name,
                "Field or sub-field '%s' for command '%s' was removed from the new definition of the"
                "command." % (field_name, command_name), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_FIELD_MISSING, command_name,
                "The command '%s' or its sub-struct has type '%s' that is missing a "
                "field '%s' that exists in the old definition of the command/struct." %
                (command_name, type_name, field_name), file)

    def add_new_param_or_command_type_field_required_error(self, command_name: str, field_name: str,
                                                           file: str, type_name: Optional[str],
                                                           is_command_parameter: bool) -> None:
        """
        Add a required parameter or command type field error.

        Add an error about the new command parameter or command type field being required when
        the corresponding old command parameter or command type field is optional.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_COMMAND_PARAMETER_REQUIRED, command_name,
                "'%s' has a required field or sub-field '%s' that was optional in the old "
                "definition of the struct." % (command_name, field_name), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRED, command_name,
                "'%s' or its sub-struct has type '%s' with a required type field '%s' "
                "that was optional in the old definition of the struct type." %
                (command_name, type_name, field_name), file)

    def add_new_param_or_command_type_field_stable_required_no_default_error(
            self, struct_name: str, field_name: str, file: str, type_name: Optional[str],
            is_command_parameter: bool) -> None:
        # pylint: disable=invalid-name
        """
        Add a stable required parameter or command type field error.

        Add an error about the new command parameter or command type field being stable and
        required with no default value when the corresponding old command parameter or command
        type field is unstable.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_COMMAND_PARAMETER_STABLE_REQUIRED_NO_DEFAULT, struct_name,
                "'%s' has a stable required field '%s' with no default that was unstable in the"
                " old definition of the struct."
                "The new definition of the field should be optional or have a default value" %
                (struct_name, field_name), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_FIELD_STABLE_REQUIRED_NO_DEFAULT, struct_name,
                ("'%s' has type '%s' with a stable and required type field '%s' with no default "
                 "that was unstable in the old definition of the struct type."
                 "The new definition of the field should be optional or have a default value") %
                (struct_name, type_name, field_name), file)

    def add_new_param_or_command_type_field_unstable_error(self, command_name: str, field_name: str,
                                                           file: str, type_name: Optional[str],
                                                           is_command_parameter: bool) -> None:
        """
        Add an unstable parameter or command type field error.

        Add an error about the new command parameter or command type field being unstable
        when the corresponding old command parameter or command type field is stable.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_COMMAND_PARAMETER_UNSTABLE, command_name,
                "'%s' has an unstable field or sub-field '%s' that was stable in the old definition"
                " of the struct." % (command_name, field_name), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_FIELD_UNSTABLE, command_name,
                "'%s' or its sub-struct has type '%s' with an unstable "
                "field '%s' that was stable in the old definition of the "
                "struct type." % (command_name, type_name, field_name), file)

    def add_new_command_or_param_type_not_enum_error(
            self, command_name: str, new_type: str, old_type: str, file: str,
            field_name: Optional[str], is_command_parameter: bool) -> None:
        """
        Add an not enum parameter or command type field error.

        Add an error about the new command or parameter type not being an enum when
        the old one is.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_NOT_ENUM, command_name,
                "The '%s' command has field or sub-field '%s' of type '%s' that is "
                "not an enum while the old definition of the field type was an enum of type '%s'." %
                (command_name, field_name, new_type, old_type), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_NOT_ENUM, command_name,
                "'%s' or its sub-struct has type '%s' that is not an enum while the old definition of the type was an enum of type '%s'."
                % (command_name, new_type, old_type), file)

    def add_new_command_or_param_type_not_struct_error(
            self, command_name: str, new_type: str, old_type: str, file: str,
            field_name: Optional[str], is_command_parameter: bool) -> None:
        """Add an error about the new command or parameter type not being a struct when the old one is."""
        if is_command_parameter:
            self._add_error(
                ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_NOT_STRUCT, command_name,
                "The '%s' command has field or sub-field '%s' of type '%s' that is "
                "not a struct while the old definition of the "
                "field type was a struct of type '%s'." % (command_name, field_name, new_type,
                                                           old_type), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_NOT_STRUCT, command_name,
                "'%s' or its sub-struct has type '%s' that is not a "
                "struct while the old definition of the type was a struct of type '%s'." %
                (command_name, new_type, old_type), file)

    def add_new_command_or_param_type_not_variant_type_error(self, command_name: str, new_type: str,
                                                             file: str, field_name: Optional[str],
                                                             is_command_parameter: bool) -> None:
        # pylint: disable=invalid-name
        """
        Add an error about the new command or parameter type not being a variant type.

        Add an error about the new command or parameter type not being a variant type
        when the old type is variant.
        """

        if is_command_parameter:
            self._add_error(ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_NOT_VARIANT, command_name,
                            ("The '%s' command has field or sub-field '%s' of type '%s' that is "
                             "not variant while the older definition of the field type is variant.")
                            % (command_name, field_name, new_type), file)
        else:
            self._add_error(ERROR_ID_NEW_COMMAND_TYPE_NOT_VARIANT, command_name,
                            ("'%s' or its sub-struct has type '%s' that is not "
                             "variant while the "
                             "older definition of the type is variant.") % (command_name, new_type),
                            file)

    def add_new_command_or_param_variant_type_not_superset_error(
            self, command_name: str, variant_type_name: str, file: str, field_name: Optional[str],
            is_command_parameter: bool) -> None:
        # pylint: disable=invalid-name
        """
        Add an error about the new variant types not being a superset.

        Add an error about the new command or parameter variant types not being a superset
        of the old variant types.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_NEW_COMMAND_PARAMETER_VARIANT_TYPE_NOT_SUPERSET, command_name,
                ("The '%s' command has field or sub-field '%s' of variant types that is not "
                 "a superset of the older definition of the field variant types: "
                 "The type '%s' is in the old definition of the field types but not the new "
                 "definition of the field types.") % (command_name, field_name, variant_type_name),
                file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_VARIANT_TYPE_NOT_SUPERSET, command_name,
                ("'%s' or its sub-struct has variant types that is not a supserset "
                 "of the older definition of the command variant types: The type '%s' "
                 "is in the old definition of the command types but not the new definition of "
                 "the command types.") % (command_name, variant_type_name), file)

    def add_new_command_or_param_chained_type_not_superset_error(
            self, command_name: str, chained_type_name: str, file: str, field_name: Optional[str],
            is_command_parameter: bool) -> None:
        # pylint: disable=invalid-name
        """
        Add an error about the new chained types not being a superset.

        Add an error about the new command or parameter chained types not being a superset
        of the old chained types.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_NEW_COMMAND_PARAMETER_CHAINED_TYPE_NOT_SUPERSET, command_name,
                ("The '%s' command has field or sub-field '%s' of chained types that is not "
                 "a superset of the corresponding old definition of the field's chained types: "
                 "The type '%s' is in the old definition of the field types but not the new "
                 "definition of the field types.") % (command_name, field_name, chained_type_name),
                file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_CHAINED_TYPE_NOT_SUPERSET, command_name,
                ("'%s' or its sub-struct has chained types that is not a supserset "
                 "of the corresponding old definition of the command chained types: The type '%s' "
                 "is in the old definition of the command types but not the new definition of the "
                 "command types.") % (command_name, chained_type_name), file)

    def add_new_namespace_incompatible_error(self, command_name: str, old_namespace: str,
                                             new_namespace: str, file: str) -> None:
        """Add an error about the new namespace being incompatible with the old namespace."""
        self._add_error(
            ERROR_ID_NEW_NAMESPACE_INCOMPATIBLE, command_name,
            "The new definition of '%s' has namespace '%s' that is incompatible with the old definition "
            " of the command with namespace '%s'." % (command_name, new_namespace, old_namespace),
            file)

    def add_new_reply_field_missing_error(self, command_name: str, field_name: str,
                                          file: str) -> None:
        """Add an error about the new command missing a reply field that exists in the old command."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_MISSING, command_name,
            "'%s' is missing a reply field or sub-field '%s' that exists in the old definition of "
            "the command." % (command_name, field_name), file)

    def add_new_reply_field_optional_error(self, command_name: str, field_name: str,
                                           file: str) -> None:
        """Add an error about the new command reply field being optional when the old reply field is not."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_OPTIONAL, command_name,
            "'%s' has an optional reply field or sub-field '%s' "
            "that was non-optional in the old definition of the command." % (command_name,
                                                                             field_name), file)

    def add_new_reply_field_bson_any_error(self, command_name: str, field_name: str,
                                           old_field_type: str, new_field_type: str,
                                           file: str) -> None:
        """
        Add an error about the new reply field type's 'any' bson serialization type.

        Add an error about the new reply field type's bson serialization type being of type
        'any' when it was not 'any' in the old type or it is not explicitly allowed.
        """
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY, command_name,
            ("The new definition of '%s' has a reply field or sub-field '%s' of type '%s' "
             "that has a bson serialization type 'any', while the old definition"
             " had type '%s' that did not have bson serialization type 'any'") %
            (command_name, field_name, new_field_type, old_field_type), file)

    def add_old_reply_field_bson_any_not_allowed_error(self, command_name: str, field_name: str,
                                                       type_name: str, file: str) -> None:
        """
        Add an error about the old reply field bson serialization type being 'any'.

        Add an error about the reply field type's bson serialization type being of
        type 'any' when it is not explicitly allowed.
        """
        self._add_error(
            ERROR_ID_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED, command_name,
            ("The old definition of '%s' has a reply field or sub-field '%s' of type '%s' "
             "that has a bson serialization type 'any' when it "
             "is not explicitly allowed.") % (command_name, field_name, type_name), file)

    def add_new_reply_field_bson_any_not_allowed_error(self, command_name: str, field_name: str,
                                                       type_name: str, file: str) -> None:
        """
        Add an error about the new reply field bson serialization type being 'any'.

        Add an error about the reply field type's bson serialization type being of
        type 'any' when it is not explicitly allowed.
        """
        self._add_error(
            ERROR_ID_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED, command_name,
            ("The new definition of '%s' has a reply field or sub-field '%s' of type '%s' "
             "that has a bson serialization type 'any' when it "
             "is not explicitly allowed.") % (command_name, field_name, type_name), file)

    def add_reply_field_cpp_type_not_equal_error(self, command_name: str, field_name: str,
                                                 type_name: str, file: str) -> None:
        """Add an error about the old and new reply field cpp_type not being equal."""
        self._add_error(ERROR_ID_REPLY_FIELD_CPP_TYPE_NOT_EQUAL, command_name,
                        ("'%s' has a reply field or sub-field '%s' of type '%s' that has cpp_type "
                         "that is not equal in the old and new definitions of this command.") %
                        (command_name, field_name, type_name), file)

    def add_reply_field_serializer_not_equal_error(self, command_name: str, field_name: str,
                                                   type_name: str, file: str) -> None:
        """Add an error about the old and new reply field serializer not being equal."""
        self._add_error(
            ERROR_ID_REPLY_FIELD_SERIALIZER_NOT_EQUAL, command_name,
            ("'%s' has a reply field or sub-field '%s' of type '%s' that has "
             "serializer that is not equal in the old and new definitions of this command.") %
            (command_name, field_name, type_name), file)

    def add_reply_field_deserializer_not_equal_error(self, command_name: str, field_name: str,
                                                     type_name: str, file: str) -> None:
        """Add an error about the old and new reply field deserializer not being equal."""
        self._add_error(
            ERROR_ID_REPLY_FIELD_DESERIALIZER_NOT_EQUAL, command_name,
            ("'%s' has a reply field or sub-field '%s' of type '%s' that has "
             "deserializer that is not equal in the old and new definitions of this command.") %
            (command_name, field_name, type_name), file)

    def add_new_reply_field_type_not_enum_error(self, command_name: str, field_name: str,
                                                new_field_type: str, old_field_type: str,
                                                file: str) -> None:
        """Add an error about the new reply field type not being an enum when the old one is."""
        self._add_error(ERROR_ID_NEW_REPLY_FIELD_TYPE_NOT_ENUM, command_name,
                        ("'%s' has a reply field or sub-field '%s' of type '%s' "
                         "that is not an enum while the corresponding "
                         "old definition of the reply field was an enum of type '%s'.") %
                        (command_name, field_name, new_field_type, old_field_type), file)

    def add_new_reply_field_type_not_struct_error(self, command_name: str, field_name: str,
                                                  new_field_type: str, old_field_type: str,
                                                  file: str) -> None:
        """Add an error about the new reply field type not being a struct when the old one is."""
        self._add_error(ERROR_ID_NEW_REPLY_FIELD_TYPE_NOT_STRUCT, command_name,
                        ("'%s' has a reply field or sub-field '%s' of type '%s' "
                         "that is not a struct while the corresponding "
                         "old definition of the reply field was a struct of type '%s'.") %
                        (command_name, field_name, new_field_type, old_field_type), file)

    def add_new_reply_field_type_enum_or_struct_error(self, command_name: str, field_name: str,
                                                      new_field_type: str, old_field_type: str,
                                                      file: str) -> None:
        """
        Add an error about a reply field type being incompatible with the old field type.

        Add an error when the new reply field type is an enum or struct
        and the old reply field is a non-enum or struct type.
        """
        self._add_error(ERROR_ID_NEW_REPLY_FIELD_TYPE_ENUM_OR_STRUCT, command_name,
                        ("'%s' has a reply field or sub-field '%s' of type '%s' that is an "
                         "enum or struct while the corresponding "
                         "old definition of the reply field was a non-enum or struct of type '%s'.")
                        % (command_name, field_name, new_field_type, old_field_type), file)

    def add_new_reply_field_unstable_error(self, command_name: str, field_name: str,
                                           file: str) -> None:
        """Add an error about the new command reply field being unstable when the old one is stable."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_UNSTABLE, command_name,
            "'%s' has an unstable reply field or sub-field '%s' "
            "that was stable in the old definition of the command." % (command_name, field_name),
            file)

    def add_new_reply_field_variant_type_error(self, command_name: str, field_name: str,
                                               old_field_type: str, file: str) -> None:
        """Add an error about the new reply field type being variant when the old one is not."""
        self._add_error(ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE, command_name,
                        ("'%s' has a reply field or sub-field '%s' that has a variant "
                         "type while the corresponding "
                         "old definition of the reply field type '%s' is not variant.") %
                        (command_name, field_name, old_field_type), file)

    def add_new_reply_field_variant_type_not_subset_error(
            self, command_name: str, field_name: str, variant_type_name: str, file: str) -> None:
        """
        Add an error about the reply field variant types not being a subset.

        Add an error about the new reply field variant types
        not being a subset of the old variant types.
        """
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET, command_name,
            ("'%s' has a reply field or sub-field '%s' with variant types that is "
             "not a subset of the corresponding "
             "old definition of the reply field types: The type '%s' is not in the old definition "
             "of the reply field types.") % (command_name, field_name, variant_type_name), file)

    def add_new_reply_chained_type_not_subset_error(self, command_name: str, reply_name: str,
                                                    chained_type_name: str, file: str) -> None:
        """
        Add an error about the reply chained types not being a subset.

        Add an error about the new reply chained types
        not being a subset of the old chained types.
        """
        self._add_error(
            ERROR_ID_NEW_REPLY_CHAINED_TYPE_NOT_SUBSET, command_name,
            ("'%s' has a reply '%s' with chained types that is "
             "not a subset of the corresponding "
             "old definition of the reply chained types: The type '%s' is not in the old "
             "definition of the reply chained types.") % (command_name, reply_name,
                                                          chained_type_name), file)

    def add_old_command_or_param_type_bson_any_error(
            self, command_name: str, old_type: str, new_type: str, file: str,
            field_name: Optional[str], is_command_parameter: bool) -> None:
        """
        Add an error about BSON serialization type.

        Add an error about the old command or command parameter type's
        bson serialization type being of type 'any' when the new type is non-any or
        when it is not explicitly allowed.
        """
        if is_command_parameter:
            self._add_error(
                ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY, command_name,
                "The old definition of the '%s' command has field or sub-field '%s' that has type '%s' "
                "that has a bson serialization type 'any', while the new definition of the command"
                " has type '%s' that does not have bson serialization type 'any'" %
                (command_name, field_name, old_type, new_type), file)
        else:
            self._add_error(ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY, command_name, (
                "The old definition of the command '%s' or its sub-struct has type '%s' that has a "
                "bson serialization type 'any', while the new definition has type '%s' "
                "that does not have bson serialization type 'any'") % (command_name, old_type,
                                                                       new_type), file)

    def add_old_command_or_param_type_bson_any_not_allowed_error(
            self, command_name: str, type_name: str, file: str, field_name: Optional[str],
            is_command_parameter: bool) -> None:
        # pylint: disable=invalid-name
        """
        Add an error about the old command or param type bson serialization type being 'any'.

        Add an error about the command or parameter type's bson serialization type
        being of type 'any' when it is not explicitly allowed.
        """
        if is_command_parameter:
            self._add_error(ERROR_ID_COMMAND_PARAMETER_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED,
                            command_name,
                            ("The old definition of '%s' has a field or sub-field '%s' of type "
                             "'%s' that has a bson "
                             "serialization type 'any' when it is not explicitly allowed.") %
                            (command_name, field_name, type_name), file)
        else:
            self._add_error(
                ERROR_ID_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED, command_name,
                ("The old definition of '%s' or its sub-struct has a type '%s' that has a bson "
                 "serialization type 'any' when it is not explicitly allowed.") % (command_name,
                                                                                   type_name), file)

    def add_new_command_or_param_type_bson_any_not_allowed_error(
            self, command_name: str, type_name: str, file: str, field_name: Optional[str],
            is_command_parameter: bool) -> None:
        # pylint: disable=invalid-name
        """
        Add an error about the new command or param type bson serialization type being 'any'.

        Add an error about the command or parameter type's bson serialization type
        being of type 'any' when it is not explicitly allowed.
        """
        if is_command_parameter:
            self._add_error(ERROR_ID_COMMAND_PARAMETER_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED,
                            command_name,
                            ("The new definition of '%s' has a field or sub-field '%s' of type "
                             "'%s' that has a bson "
                             "serialization type 'any' when it is not explicitly allowed.") %
                            (command_name, field_name, type_name), file)
        else:
            self._add_error(
                ERROR_ID_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED, command_name,
                ("The new definition of '%s' or its sub-struct has a type '%s' that has a bson "
                 "serialization type 'any' when it is not explicitly allowed.") % (command_name,
                                                                                   type_name), file)

    def add_command_or_param_cpp_type_not_equal_error(self, command_name: str, type_name: str,
                                                      file: str, field_name: Optional[str],
                                                      is_command_parameter: bool) -> None:
        """Add an error about the old and new command or param cpp_type not being equal."""
        if is_command_parameter:
            self._add_error(ERROR_ID_COMMAND_PARAMETER_CPP_TYPE_NOT_EQUAL, command_name,
                            ("'%s' has field or sub-field '%s' of type '%s' that has  "
                             "cpp_type that is not equal in the old and new definitions") %
                            (command_name, field_name, type_name), file)
        else:
            self._add_error(
                ERROR_ID_COMMAND_CPP_TYPE_NOT_EQUAL, command_name,
                ("'%s' or its sub-struct has command type '%s' that has cpp_type "
                 "that is not equal in the old and new definitions") % (command_name, type_name),
                file)

    def add_command_or_param_serializer_not_equal_error(self, command_name: str, type_name: str,
                                                        file: str, field_name: Optional[str],
                                                        is_command_parameter: bool) -> None:
        """Add an error about the old and new command or param serializer not being equal."""
        if is_command_parameter:
            self._add_error(ERROR_ID_COMMAND_PARAMETER_SERIALIZER_NOT_EQUAL, command_name,
                            ("'%s' has field or sub-field '%s' of type '%s' that has  "
                             "serializer that is not equal in the old and new definitions") %
                            (command_name, field_name, type_name), file)
        else:
            self._add_error(
                ERROR_ID_COMMAND_SERIALIZER_NOT_EQUAL, command_name,
                ("'%s' or its sub-struct has command type '%s' that has serializer "
                 "that is not equal in the old and new definitions") % (command_name, type_name),
                file)

    def add_command_or_param_deserializer_not_equal_error(self, command_name: str, type_name: str,
                                                          file: str, field_name: Optional[str],
                                                          is_command_parameter: bool) -> None:
        """Add an error about the old and new command or param deserializer not being equal."""
        if is_command_parameter:
            self._add_error(ERROR_ID_COMMAND_PARAMETER_DESERIALIZER_NOT_EQUAL, command_name,
                            ("'%s' has field or sub-field '%s' of type '%s' that has  "
                             "deserializer that is not equal in the old and new definitions") %
                            (command_name, field_name, type_name), file)
        else:
            self._add_error(
                ERROR_ID_COMMAND_DESERIALIZER_NOT_EQUAL, command_name,
                ("'%s' or its sub-struct has command type '%s' that has deserializer "
                 "that is not equal in the old and new definitions") % (command_name, type_name),
                file)

    def add_old_reply_field_bson_any_error(self, command_name: str, field_name: str,
                                           old_field_type: str, new_field_type: str,
                                           file: str) -> None:
        """
        Add an about the old reply field type's 'any' bson serialization type.

        Add an error about the old reply field type's bson serialization type being of type
        'any' when the new type is non-any or when it is not explicitly allowed.
        """
        self._add_error(
            ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY, command_name,
            ("The old definition of '%s' has a reply field or sub-field '%s' of type '%s' "
             "that has a bson serialization type 'any', while the new definition has"
             " type '%s' that does not have bson serialization type 'any'") %
            (command_name, field_name, old_field_type, new_field_type), file)

    def add_reply_field_contains_validator_error(self, command_name: str, field_name: str,
                                                 file: str) -> None:
        """Add an error about the reply field containing a validator."""
        self._add_error(
            ERROR_ID_REPLY_FIELD_CONTAINS_VALIDATOR, command_name,
            ("The new definition of the command '%s' has a reply field or sub-field '%s' "
             "that contains a validator while the old definition does not") % (command_name,
                                                                               field_name), file)

    def add_reply_field_validators_not_equal_error(self, command_name: str, field_name: str,
                                                   file: str) -> None:
        """Add an error about the reply field containing a validator."""
        self._add_error(
            ERROR_ID_REPLY_FIELD_VALIDATORS_NOT_EQUAL, command_name,
            ("Validator for reply field or sub-field '%s' in old definition of command '%s' "
             "is not equal to the validator in the new definition of the reply field") %
            (command_name, field_name), file)

    def add_reply_field_type_invalid_error(self, command_name: str, field_name: str,
                                           file: str) -> None:
        """Add an error about the reply field or sub-field type being invalid."""
        self._add_error(ERROR_ID_REPLY_FIELD_TYPE_INVALID, command_name,
                        ("'%s' has a reply field or sub-field '%s' that has an invalid type") %
                        (command_name, field_name), file)

    def add_check_not_equal_error(self, command_name: str, old_check: str, new_check: str,
                                  file: str) -> None:
        """Add an error about the command access_check check not being equal."""
        self._add_error(ERROR_ID_CHECK_NOT_EQUAL, command_name, (
            "The new definition of '%s' has a check '%s' that is not equal to the check '%s' in the old definition"
            " of the command.") % (command_name, new_check, old_check), file)

    def add_resource_pattern_not_equal_error(self, command_name: str, old_resource_pattern: str,
                                             new_resource_pattern: str, file: str) -> None:
        """Add an error about the command access_check resource_pattern not being equal."""
        self._add_error(
            ERROR_ID_RESOURCE_PATTERN_NOT_EQUAL, command_name,
            ("The new definition of '%s' has a resource pattern '%s' that is not equal to the "
             "the resource pattern '%s' in the old definition of the command.") %
            (command_name, new_resource_pattern, old_resource_pattern), file)

    def add_new_action_types_not_subset_error(self, command_name: str, file: str) -> None:
        """Add an error about the new access_check action types not being a subset of the old ones."""
        self._add_error(
            ERROR_ID_NEW_ACTION_TYPES_NOT_SUBSET, command_name,
            ("The new definition of '%s' has action types that are not a subset of the action"
             " types in the old definition") % (command_name), file)

    def add_type_not_array_error(self, symbol: str, command_name: str, symbol_name: str,
                                 new_type: str, old_type: str, file: str) -> None:
        """
        Add an error about type not being an ArrayType when it should be.

        This is a general error for each case where ArrayType is missing from (command type,
        command parameter type).
        """
        self._add_error(
            ERROR_ID_TYPE_NOT_ARRAY, command_name,
            "The new definition of command '%s' has %s: '%s' with type '%s' that is not an ArrayType while the old definition had type '%s' which was an ArrayType."
            % (command_name, symbol, symbol_name, new_type, old_type), file)

    def add_access_check_type_not_equal_error(self, command_name: str, old_type: str, new_type: str,
                                              file: str) -> None:
        """Add an error about the command access_check types not being equal."""
        self._add_error(ERROR_ID_ACCESS_CHECK_TYPE_NOT_EQUAL, command_name, (
            "'%s' has access_check of type %s in the old definition that is not equal to the access_check of type '%s'"
            "in the new definition.") % (command_name, old_type, new_type), file)

    def add_new_complex_checks_not_subset_error(self, command_name: str, file: str) -> None:
        """Add an error about the complex access_check checks not being a subset of the old ones."""
        self._add_error(
            ERROR_ID_NEW_COMPLEX_CHECKS_NOT_SUBSET, command_name,
            ("The new definition of '%s' has complex access_checks checks that are not a subset "
             " of the old definition's complex"
             " access_check checks.") % (command_name), file)

    def add_new_complex_privileges_not_subset_error(self, command_name: str, file: str) -> None:
        """Add an error about the complex access_check privileges not being a subset of the old ones."""
        self._add_error(
            ERROR_ID_NEW_COMPLEX_PRIVILEGES_NOT_SUBSET, command_name,
            ("'%s' has complex access_checks privileges that have changed the resource_pattern"
             " or changed/added an action_type in the new definition of the command.") %
            (command_name), file)

    def add_new_additional_complex_access_check_error(self, command_name: str, file: str) -> None:
        """Add an error about an additional complex access_check being added."""
        self._add_error(
            ERROR_ID_NEW_ADDITIONAL_COMPLEX_ACCESS_CHECK, command_name,
            ("'%s' has additional complex access_checks in the new definition of the command that "
             "are not in the old definition of the command") % (command_name), file)

    def add_removed_access_check_field_error(self, command_name: str, file: str) -> None:
        """Add an error the new command removing the access_check field."""
        self._add_error(
            ERROR_ID_REMOVED_ACCESS_CHECK_FIELD, command_name,
            ("'%s' has removed the access_check field in the new definition of the command when it "
             "exists in the old definition of the command") % (command_name), file)

    def add_added_access_check_field_error(self, command_name: str, file: str) -> None:
        """Add an error the new command adding the access_check field when the api_version is '1'."""
        self._add_error(ERROR_ID_ADDED_ACCESS_CHECK_FIELD, command_name, (
            "The new definition of '%s' has added the access_check field when it did not exist in the "
            "old definition of the command, while the api_version is '1'") % (command_name), file)

    def add_generic_argument_removed(self, field_name: str, file: str) -> None:
        """Add an error about a generic argument that was removed."""
        self._add_error(ERROR_ID_GENERIC_ARGUMENT_REMOVED, field_name,
                        ("The generic argument '%s' was removed from the new definition of the "
                         "generic_argument.idl file") % (field_name), file)

    def add_generic_argument_removed_reply_field(self, field_name: str, file: str) -> None:
        """Add an error about a generic reply field that was removed."""
        self._add_error(ERROR_ID_GENERIC_ARGUMENT_REMOVED_REPLY_FIELD, field_name,
                        ("The generic reply field '%s' was removed from the new definition of the "
                         "generic_argument.idl file") % (field_name), file)

    def add_new_reply_field_requires_stability_error(self, command_name: str, field_name: str,
                                                     file: str) -> None:
        """Add an error that a new reply field requires the 'stability' field."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_REQUIRES_STABILITY, command_name,
            ("The new definition of '%s' has reply field '%s' that requires specifying a value "
             "for the 'stability' field") % (command_name, field_name), file)

    def add_new_param_or_command_type_field_requires_stability_error(
            self, command_name: str, field_name: str, file: str,
            is_command_parameter: bool) -> None:
        # pylint: disable=invalid-name
        """Add an error that a new param or command type field requires the 'stability' field."""
        if is_command_parameter:
            self._add_error(
                ERROR_ID_NEW_PARAMETER_REQUIRES_STABILITY, command_name,
                ("The new definition of '%s' has parameter '%s' that requires specifying a value "
                 "for the 'stability' field") % (command_name, field_name), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRES_STABILITY, command_name,
                ("The new definition of '%s' has command type field '%s' that requires specifying "
                 "a value for the 'stability' field") % (command_name, field_name), file)

    def add_unstable_reply_field_changed_to_stable_error(self, command_name: str, field_name: str,
                                                         file: str) -> None:
        """Add an error that a reply field may not change from unstable to stable."""
        self._add_error(ERROR_ID_UNSTABLE_REPLY_FIELD_CHANGED_TO_STABLE, command_name, (
            "The command '%s' has reply field '%s' which is unstable and may not be changed to stable in "
            "the new definition unless explicitly allowed.") % (command_name, field_name), file)

    def add_unstable_param_or_type_field_to_stable_error(self, command_name: str, field_name: str,
                                                         file: str,
                                                         is_command_parameter: bool) -> None:
        """Add an error that a command parameter or type field may not change from unstable to stable."""
        if is_command_parameter:
            self._add_error(
                ERROR_ID_UNSTABLE_COMMAND_PARAM_FIELD_CHANGED_TO_STABLE, command_name,
                ("The command '%s' has command parameter field '%s' which is unstable and may "
                 "not be changed to stable in the new definition unless explicitly allowed.") %
                (command_name, field_name), file)
        else:
            self._add_error(
                ERROR_ID_UNSTABLE_COMMAND_TYPE_FIELD_CHANGED_TO_STABLE, command_name,
                ("The command '%s' has command type field '%s' which is unstable and may "
                 "not be changed to stable in the new definition unless explicitly allowed.") %
                (command_name, field_name), file)

    def add_new_reply_field_added_as_stable_error(self, command_name: str, field_name: str,
                                                  file: str) -> None:
        """Add an error that a new reply field may not be added as stable unless explicitly allowed."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_ADDED_AS_STABLE, command_name,
            ("The command '%s' has newly-added reply field '%s' which may not be defined as stable "
             "unless that addition is explicitly allowed.") % (command_name, field_name), file)

    def add_new_param_or_type_field_added_as_stable_error(self, command_name: str, field_name: str,
                                                          file: str,
                                                          is_command_parameter: bool) -> None:
        """Add an error that a new command param or type field may not be added as stable unless explicitly allowed."""
        if is_command_parameter:
            self._add_error(
                ERROR_ID_NEW_COMMAND_PARAM_FIELD_ADDED_AS_STABLE, command_name,
                ("The command '%s' has newly-added param '%s' which may not be defined as stable "
                 "unless that addition is explicitly allowed.") % (command_name, field_name), file)
        else:
            self._add_error(
                ERROR_ID_NEW_COMMAND_TYPE_FIELD_ADDED_AS_STABLE, command_name,
                ("The command '%s' has newly-added type '%s' which may not be defined as stable "
                 "unless that addition is explicitly allowed.") % (command_name, field_name), file)


def _assert_unique_error_messages() -> None:
    """Assert that error codes are unique."""
    error_ids = []
    for module_member in inspect.getmembers(sys.modules[__name__]):
        if module_member[0].startswith("ERROR_ID"):
            error_ids.append(module_member[1])

    error_ids_set = set(error_ids)
    if len(error_ids) != len(error_ids_set):
        raise IDLCompatibilityCheckerError(
            "IDL Compatibility Checker error codes prefixed with ERROR_ID are not unique.")


# On file import, check the error messages are unique
_assert_unique_error_messages()
