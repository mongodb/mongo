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
from typing import List

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
ERROR_ID_COMMAND_NOT_SUBSET = "ID0013"
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

    #pylint: disable=too-many-arguments
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
        msg = "Comparing %s and %s: Error in %s: %s: %s" % (os.path.basename(self.old_idl_dir),
                                                            os.path.basename(self.new_idl_dir),
                                                            self.file, self.error_id, self.msg)
        return msg


class IDLCompatibilityErrorCollection(object):
    """Collection of IDL compatibility errors with source context information."""

    def __init__(self) -> None:
        """Initialize IDLCompatibilityErrorCollection."""
        self._errors: List[IDLCompatibilityError] = []

    #pylint: disable=too-many-arguments
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

    def to_list(self) -> List[str]:
        """Return a list of formatted error messages."""
        return [str(error) for error in self._errors]

    def dump_errors(self) -> None:
        """Print the list of errors."""
        print("Errors found while checking IDL compatibility")
        for error_msg in self.to_list():
            print("%s\n\n" % error_msg)
        print("Found %s errors" % (len(self.to_list())))

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

    # pylint:disable=too-many-public-methods

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
        self._add_error(ERROR_ID_REMOVED_COMMAND, command_name,
                        "Old command '%s' was removed from new commands." % (command_name), file)

    def add_duplicate_command_name_error(self, command_name: str, dir_name: str, file: str) -> None:
        """Add an error about a duplicate command name within a directory."""
        self._add_error(ERROR_ID_DUPLICATE_COMMAND_NAME, command_name,
                        "'%s' has duplicate command: '%s'" % (dir_name, command_name), file)

    def add_command_not_subset_error(self, command_name: str, field_name: str, type_name: str,
                                     file: str) -> None:
        """Add an error about the command not being a subset."""
        self._add_error(
            ERROR_ID_COMMAND_NOT_SUBSET, command_name,
            "'%s' has field '%s' with type '%s' that is not a subset of the other version of this command."
            % (command_name, field_name, type_name), file)

    def add_command_type_invalid_error(self, command_name: str, file: str) -> None:
        """Add an error about the command type being invalid."""
        self._add_error(ERROR_ID_COMMAND_TYPE_INVALID, command_name,
                        ("'%s' has an invalid type") % (command_name), file)

    def add_command_type_not_superset_error(self, command_name: str, type_name: str,
                                            file: str) -> None:
        """Add an error about the command type not being a subset."""
        self._add_error(
            ERROR_ID_COMMAND_TYPE_NOT_SUPERSET, command_name,
            "'%s' has type '%s' that is not a subset of the other version of this command." %
            (command_name, type_name), file)

    def add_new_command_type_bson_any_error(self, command_name: str, new_type: str,
                                            file: str) -> None:
        """Add an error about the new command type's bson serialization type being of type "any"."""
        self._add_error(ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY, command_name,
                        ("'%s' has type '%s' that has a bson serialization type 'any'") %
                        (command_name, new_type), file)

    def add_new_command_type_enum_or_struct_error(self, command_name: str, new_type: str,
                                                  old_type: str, file: str) -> None:
        # pylint: disable=too-many-arguments
        """Add an error when the new command type is an enum or struct and the old one is a non-enum or struct type."""
        self._add_error(ERROR_ID_NEW_COMMAND_TYPE_ENUM_OR_STRUCT, command_name,
                        ("'%s' has type '%s' that is an enum or struct while the corresponding "
                         "old type was a non-enum or struct of type '%s'.") %
                        (command_name, new_type, old_type), file)

    def add_new_command_type_field_missing_error(self, command_name: str, type_name: str,
                                                 field_name: str, file: str) -> None:
        """Add an error about the new command type missing a field that exists in the old command."""
        self._add_error(
            ERROR_ID_NEW_COMMAND_TYPE_FIELD_MISSING, command_name,
            "'%s' has type '%s' that is missing a field '%s' that exists in the old command." %
            (command_name, type_name, field_name), file)

    def add_new_command_type_field_required_error(self, command_name: str, type_name: str,
                                                  field_name: str, file: str) -> None:
        """Add an error about the new command type field being required when the old type field is not."""
        self._add_error(
            ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRED, command_name,
            "'%s' has type '%s' with a required type field '%s' that was optional in the old command."
            % (command_name, type_name, field_name), file)

    def add_new_command_type_field_unstable_error(self, command_name: str, type_name,
                                                  field_name: str, file: str) -> None:
        """Add an error about the new command type field being unstable when the old one is stable."""
        self._add_error(
            ERROR_ID_NEW_COMMAND_TYPE_FIELD_UNSTABLE, command_name,
            "'%s' has type '%s' with an unstable field '%s' that was stable in the old command." %
            (command_name, type_name, field_name), file)

    def add_new_command_type_not_enum_error(self, command_name: str, new_type: str, old_type: str,
                                            file: str) -> None:
        # pylint: disable=too-many-arguments
        """Add an error about the new command type not being an enum when the old one is."""
        self._add_error(ERROR_ID_NEW_COMMAND_TYPE_NOT_ENUM, command_name,
                        ("'%s' has type '%s' that is not an enum while the corresponding "
                         "old type was an enum of type '%s'.") % (command_name, new_type, old_type),
                        file)

    def add_new_command_type_not_struct_error(self, command_name: str, new_type: str, old_type: str,
                                              file: str) -> None:
        # pylint: disable=too-many-arguments
        """Add an error about the new command type not being a struct when the old one is."""
        self._add_error(
            ERROR_ID_NEW_COMMAND_TYPE_NOT_STRUCT, command_name,
            ("'%s' has type '%s' that is not a struct while the corresponding "
             "old type was a struct of type '%s'.") % (command_name, new_type, old_type), file)

    def add_new_namespace_incompatible_error(self, command_name: str, old_namespace: str,
                                             new_namespace: str, file: str) -> None:
        """Add an error about the new namespace being incompatible with the old namespace."""
        self._add_error(
            ERROR_ID_NEW_NAMESPACE_INCOMPATIBLE, command_name,
            "'%s' has namespace '%s' that is incompatible with the old namespace '%s'." %
            (command_name, new_namespace, old_namespace), file)

    def add_new_reply_field_missing_error(self, command_name: str, field_name: str,
                                          file: str) -> None:
        """Add an error about the new command missing a reply field that exists in the old command."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_MISSING, command_name,
            "'%s' is missing a reply field '%s' that exists in the old command." %
            (command_name, field_name), file)

    def add_new_reply_field_optional_error(self, command_name: str, field_name: str,
                                           file: str) -> None:
        """Add an error about the new command reply field being optional when the old reply field is not."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_OPTIONAL, command_name,
            "'%s' has an optional reply field '%s' that was non-optional in the old command." %
            (command_name, field_name), file)

    def add_new_reply_field_bson_any_error(self, command_name: str, field_name: str,
                                           new_field_type: str, file: str) -> None:
        """Add an error about the new reply field type's bson serialization type being of type "any"."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY, command_name,
            ("'%s' has a reply field '%s' of type '%s' that has a bson serialization type 'any'") %
            (command_name, field_name, new_field_type), file)

    def add_new_reply_field_type_not_enum_error(self, command_name: str, field_name: str,
                                                new_field_type: str, old_field_type: str,
                                                file: str) -> None:
        # pylint: disable=too-many-arguments
        """Add an error about the new reply field type not being an enum when the old one is."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_TYPE_NOT_ENUM, command_name,
            ("'%s' has a reply field '%s' of type '%s' that is not an enum while the corresponding "
             "old reply field was an enum of type '%s'.") % (command_name, field_name,
                                                             new_field_type, old_field_type), file)

    def add_new_reply_field_type_not_struct_error(self, command_name: str, field_name: str,
                                                  new_field_type: str, old_field_type: str,
                                                  file: str) -> None:
        # pylint: disable=too-many-arguments
        """Add an error about the new reply field type not being a struct when the old one is."""
        self._add_error(ERROR_ID_NEW_REPLY_FIELD_TYPE_NOT_STRUCT, command_name, (
            "'%s' has a reply field '%s' of type '%s' that is not a struct while the corresponding "
            "old reply field was a struct of type '%s'.") % (command_name, field_name,
                                                             new_field_type, old_field_type), file)

    def add_new_reply_field_type_enum_or_struct_error(self, command_name: str, field_name: str,
                                                      new_field_type: str, old_field_type: str,
                                                      file: str) -> None:
        # pylint: disable=too-many-arguments
        """Add an error when the new reply field type is an enum or struct and the old reply field is a non-enum or struct type."""
        self._add_error(ERROR_ID_NEW_REPLY_FIELD_TYPE_ENUM_OR_STRUCT, command_name, (
            "'%s' has a reply field '%s' of type '%s' that is an enum or struct while the corresponding "
            "old reply field was a non-enum or struct of type '%s'.") %
                        (command_name, field_name, new_field_type, old_field_type), file)

    def add_new_reply_field_unstable_error(self, command_name: str, field_name: str,
                                           file: str) -> None:
        """Add an error about the new command reply field being unstable when the old one is stable."""
        self._add_error(
            ERROR_ID_NEW_REPLY_FIELD_UNSTABLE, command_name,
            "'%s' has an unstable reply field '%s' that was stable in the old command." %
            (command_name, field_name), file)

    def add_old_command_type_bson_any_error(self, command_name: str, old_type: str,
                                            file: str) -> None:
        """Add an error about the old command type's bson serialization type being of type "any"."""
        self._add_error(ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY, command_name,
                        ("'%s' has type '%s' that has a bson serialization type 'any'") %
                        (command_name, old_type), file)

    def add_old_reply_field_bson_any_error(self, command_name: str, field_name: str,
                                           old_field_type: str, file: str) -> None:
        """Add an error about the old reply field type's bson serialization type being of type "any"."""
        self._add_error(
            ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY, command_name,
            ("'%s' has a reply field '%s' of type '%s' that has a bson serialization type 'any'") %
            (command_name, field_name, old_field_type), file)

    def add_reply_field_type_invalid_error(self, command_name: str, field_name: str,
                                           file: str) -> None:
        """Add an error about the reply field type being invalid."""
        self._add_error(
            ERROR_ID_REPLY_FIELD_TYPE_INVALID, command_name,
            ("'%s' has a reply field '%s' that has an invalid type") % (command_name, field_name),
            file)


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
