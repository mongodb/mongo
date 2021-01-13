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


class IDLCompatibilityCheckerError(Exception):
    """Base class for all IDL Compatibility Checker exceptions."""

    pass


class IDLCompatibilityError(object):
    """
    IDLCompatibilityError represents an error from the IDL compatibility checker.

    An IDLCompatibilityError consists of
    - error_id - IDxxxx where xxxx is a 0 leading number.
    - msg - a string describing an error.
    - old_idl_dir - a string, the directory containing the old IDL files.
    - new_idl_dir - a string, the directory containing the new IDL files.
    - file - a string, the path to the IDL file where the error occurred.
    """

    #pylint: disable=too-many-arguments
    def __init__(self, error_id: str, msg: str, old_idl_dir: str, new_idl_dir: str,
                 file: str) -> None:
        """Construct an IDLCompatibility error."""
        self.error_id = error_id
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
    def add(self, error_id: str, msg: str, old_idl_dir: str, new_idl_dir: str, file: str) -> None:
        """Add an error message with directory information."""
        self._errors.append(IDLCompatibilityError(error_id, msg, old_idl_dir, new_idl_dir, file))

    def has_errors(self) -> bool:
        """Have any errors been added to the collection?."""
        return len(self._errors) > 0

    def contains(self, error_id: str) -> bool:
        """Check if the error collection has at least one message of a given error_id."""
        return len([a for a in self._errors if a.error_id == error_id]) > 0

    def get_error(self, error_id: str) -> IDLCompatibilityError:
        """Get the first error in the error collection with the id error_id."""
        error_id_list = [a for a in self._errors if a.error_id == error_id]
        if error_id_list:
            return error_id_list[0]
        return None

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

    def __init__(self, old_idl_dir: str, new_idl_dir: str,
                 errors: IDLCompatibilityErrorCollection) -> None:
        """Construct a new IDLCompatibilityContext."""
        self.old_idl_dir = old_idl_dir
        self.new_idl_dir = new_idl_dir
        self.errors = errors

    def _add_error(self, error_id: str, msg: str, file: str) -> None:
        """Add an error with an error id and error message."""
        self.errors.add(error_id, msg, self.old_idl_dir, self.new_idl_dir, file)

    def add_command_invalid_api_version_error(self, command_name: str, api_version: str,
                                              file: str) -> None:
        """Add an error about a command with an invalid api version."""
        self._add_error(ERROR_ID_COMMAND_INVALID_API_VERSION,
                        "'%s' has an invalid API version '%s'" % (command_name, api_version), file)

    def add_duplicate_command_name_error(self, command_name: str, dir_name: str, file: str) -> None:
        """Add an error about a duplicate command name within a directory."""
        self._add_error(ERROR_ID_DUPLICATE_COMMAND_NAME,
                        "'%s' has duplicate command name '%s'" % (dir_name, command_name), file)


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
