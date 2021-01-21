#!/usr/bin/env python3
#
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
"""Test cases for IDL compatibility checker."""

import unittest
import sys
from os import path
sys.path.append(path.dirname(path.dirname(path.abspath(__file__))))

#pylint: disable=wrong-import-position
import idl_check_compatibility
import idl_compatibility_errors

#pylint: enable=wrong-import-position


class TestIDLCompatibilityChecker(unittest.TestCase):
    # pylint: disable=too-many-public-methods
    """Test the IDL Compatibility Checker."""

    def test_should_pass(self):
        """Tests that compatible old and new IDL commands should pass."""
        dir_path = path.dirname(path.realpath(__file__))
        self.assertFalse(
            idl_check_compatibility.check_compatibility(
                path.join(dir_path, "compatibility_test_pass/old"),
                path.join(dir_path, "compatibility_test_pass/new"), ["src"]).has_errors())

    def test_should_fail(self):
        """Tests that incompatible old and new IDL commands should fail."""
        dir_path = path.dirname(path.realpath(__file__))
        error_collection = idl_check_compatibility.check_compatibility(
            path.join(dir_path, "compatibility_test_fail/old"),
            path.join(dir_path, "compatibility_test_fail/new"), ["src"])

        self.assertTrue(error_collection.has_errors())
        self.assertTrue(error_collection.count() == 5)

        invalid_api_version_new_error = error_collection.get_error_by_command_name(
            "invalidAPIVersionNew")
        self.assertIsNotNone(invalid_api_version_new_error)
        self.assertTrue(invalid_api_version_new_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_INVALID_API_VERSION)
        self.assertRegex(str(invalid_api_version_new_error), "invalidAPIVersionNew")

        duplicate_command_new_error = error_collection.get_error_by_command_name(
            "duplicateCommandNew")
        self.assertIsNotNone(duplicate_command_new_error)
        self.assertTrue(duplicate_command_new_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_DUPLICATE_COMMAND_NAME)
        self.assertRegex(str(duplicate_command_new_error), "duplicateCommandNew")

        invalid_api_version_old_error = error_collection.get_error_by_command_name(
            "invalidAPIVersionOld")
        self.assertIsNotNone(invalid_api_version_old_error)
        self.assertTrue(invalid_api_version_old_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_INVALID_API_VERSION)
        self.assertRegex(str(invalid_api_version_old_error), "invalidAPIVersionOld")

        duplicate_command_old_error = error_collection.get_error_by_command_name(
            "duplicateCommandOld")
        self.assertIsNotNone(duplicate_command_old_error)
        self.assertTrue(duplicate_command_old_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_DUPLICATE_COMMAND_NAME)
        self.assertRegex(str(duplicate_command_old_error), "duplicateCommandOld")

        removed_command_error = error_collection.get_error(
            idl_compatibility_errors.ERROR_ID_REMOVED_COMMAND)
        self.assertIsNotNone(removed_command_error)
        self.assertRegex(str(removed_command_error), "removedCommand")


if __name__ == '__main__':
    unittest.main()
