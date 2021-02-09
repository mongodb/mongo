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
    """Test the IDL Compatibility Checker."""

    def test_should_pass(self):
        """Tests that compatible old and new IDL commands should pass."""
        dir_path = path.dirname(path.realpath(__file__))
        self.assertFalse(
            idl_check_compatibility.check_compatibility(
                path.join(dir_path, "compatibility_test_pass/old"),
                path.join(dir_path, "compatibility_test_pass/new"), ["src"]).has_errors())

    def test_should_abort(self):
        """Tests that invalid old and new IDL commands should cause script to abort."""
        dir_path = path.dirname(path.realpath(__file__))
        with self.assertRaises(SystemExit):
            idl_check_compatibility.check_compatibility(
                path.join(dir_path, "compatibility_test_fail/old_abort"),
                path.join(dir_path, "compatibility_test_fail/new_abort"), ["src"])

    # pylint: disable=too-many-locals,too-many-statements
    def test_should_fail(self):
        """Tests that incompatible old and new IDL commands should fail."""
        dir_path = path.dirname(path.realpath(__file__))
        error_collection = idl_check_compatibility.check_compatibility(
            path.join(dir_path, "compatibility_test_fail/old"),
            path.join(dir_path, "compatibility_test_fail/new"), ["src"])

        self.assertTrue(error_collection.has_errors())
        self.assertTrue(error_collection.count() == 44)

        invalid_api_version_new_error = error_collection.get_error_by_command_name(
            "invalidAPIVersionNew")
        self.assertTrue(invalid_api_version_new_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_INVALID_API_VERSION)
        self.assertRegex(str(invalid_api_version_new_error), "invalidAPIVersionNew")

        duplicate_command_new_error = error_collection.get_error_by_command_name(
            "duplicateCommandNew")
        self.assertTrue(duplicate_command_new_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_DUPLICATE_COMMAND_NAME)
        self.assertRegex(str(duplicate_command_new_error), "duplicateCommandNew")

        invalid_api_version_old_error = error_collection.get_error_by_command_name(
            "invalidAPIVersionOld")
        self.assertTrue(invalid_api_version_old_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_INVALID_API_VERSION)
        self.assertRegex(str(invalid_api_version_old_error), "invalidAPIVersionOld")

        duplicate_command_old_error = error_collection.get_error_by_command_name(
            "duplicateCommandOld")
        self.assertTrue(duplicate_command_old_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_DUPLICATE_COMMAND_NAME)
        self.assertRegex(str(duplicate_command_old_error), "duplicateCommandOld")

        removed_command_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_REMOVED_COMMAND)
        self.assertRegex(str(removed_command_error), "removedCommand")

        removed_command_parameter_error = error_collection.get_error_by_command_name(
            "removedCommandParameter")
        self.assertTrue(removed_command_parameter_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REMOVED_COMMAND_PARAMETER)
        self.assertRegex(str(removed_command_parameter_error), "removedCommandParameter")

        added_required_command_parameter_error = error_collection.get_error_by_command_name(
            "addedNewCommandParameterRequired")
        self.assertTrue(added_required_command_parameter_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_ADDED_REQUIRED_COMMAND_PARAMETER)
        self.assertRegex(
            str(added_required_command_parameter_error), "addedNewCommandParameterRequired")

        command_parameter_unstable_error = error_collection.get_error_by_command_name(
            "commandParameterUnstable")
        self.assertTrue(command_parameter_unstable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_UNSTABLE)
        self.assertRegex(str(command_parameter_unstable_error), "commandParameterUnstable")

        command_parameter_stable_required_error = error_collection.get_error_by_command_name(
            "commandParameterStableRequired")
        self.assertTrue(command_parameter_stable_required_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_STABLE_REQUIRED)
        self.assertRegex(
            str(command_parameter_stable_required_error), "commandParameterStableRequired")

        command_parameter_required_error = error_collection.get_error_by_command_name(
            "commandParameterRequired")
        self.assertTrue(command_parameter_required_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_REQUIRED)
        self.assertRegex(str(command_parameter_required_error), "commandParameterRequired")

        new_reply_field_unstable_error = error_collection.get_error_by_command_name(
            "newReplyFieldUnstable")
        self.assertTrue(new_reply_field_unstable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_UNSTABLE)
        self.assertRegex(str(new_reply_field_unstable_error), "newReplyFieldUnstable")

        new_reply_field_optional_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_OPTIONAL)
        self.assertRegex(str(new_reply_field_optional_error), "newReplyFieldOptional")

        new_reply_field_missing_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_MISSING)
        self.assertRegex(str(new_reply_field_missing_error), "newReplyFieldMissing")

        imported_reply_field_unstable_error = error_collection.get_error_by_command_name(
            "importedReplyCommand")
        self.assertTrue(imported_reply_field_unstable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_UNSTABLE)
        self.assertRegex(str(imported_reply_field_unstable_error), "importedReplyCommand")

        new_reply_field_type_enum_not_subset_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeEnumNotSubset")
        self.assertTrue(new_reply_field_type_enum_not_subset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_type_enum_not_subset_error), "newReplyFieldTypeEnumNotSubset")

        new_reply_field_type_not_enum_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_TYPE_NOT_ENUM)
        self.assertRegex(str(new_reply_field_type_not_enum_error), "newReplyFieldTypeNotEnum")

        new_reply_field_type_not_struct_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_TYPE_NOT_STRUCT)
        self.assertRegex(str(new_reply_field_type_not_struct_error), "newReplyFieldTypeNotStruct")

        new_reply_field_type_enum_or_struct_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_TYPE_ENUM_OR_STRUCT)
        self.assertRegex(
            str(new_reply_field_type_enum_or_struct_error), "newReplyFieldTypeEnumOrStruct")

        new_reply_field_type_bson_not_subset_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeBsonNotSubset")
        self.assertTrue(new_reply_field_type_bson_not_subset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_type_bson_not_subset_error), "newReplyFieldTypeBsonNotSubset")

        new_reply_field_type_bson_not_subset_two_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeBsonNotSubsetTwo")
        self.assertTrue(new_reply_field_type_bson_not_subset_two_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_type_bson_not_subset_two_error),
            "newReplyFieldTypeBsonNotSubsetTwo")

        old_reply_field_type_bson_any_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(old_reply_field_type_bson_any_error), "oldReplyFieldTypeBsonAny")

        new_reply_field_type_bson_any_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(new_reply_field_type_bson_any_error), "newReplyFieldTypeBsonAny")

        new_reply_field_type_struct_one_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeStructRecursiveOne")
        self.assertTrue(new_reply_field_type_struct_one_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_UNSTABLE)
        self.assertRegex(
            str(new_reply_field_type_struct_one_error), "newReplyFieldTypeStructRecursiveOne")

        new_reply_field_type_struct_two_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeStructRecursiveTwo")
        self.assertTrue(new_reply_field_type_struct_two_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_type_struct_two_error), "newReplyFieldTypeStructRecursiveTwo")

        new_namespace_not_ignored_error = error_collection.get_error_by_command_name(
            "newNamespaceNotIgnored")
        self.assertTrue(new_namespace_not_ignored_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_NAMESPACE_INCOMPATIBLE)
        self.assertRegex(str(new_namespace_not_ignored_error), "newNamespaceNotIgnored")

        new_namespace_not_concatenate_with_db_or_uuid_error = error_collection.get_error_by_command_name(
            "newNamespaceNotConcatenateWithDbOrUuid")
        self.assertTrue(new_namespace_not_concatenate_with_db_or_uuid_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_NAMESPACE_INCOMPATIBLE)
        self.assertRegex(
            str(new_namespace_not_concatenate_with_db_or_uuid_error),
            "newNamespaceNotConcatenateWithDbOrUuid")

        new_namespace_not_concatenate_with_db_error = error_collection.get_error_by_command_name(
            "newNamespaceNotConcatenateWithDb")
        self.assertTrue(new_namespace_not_concatenate_with_db_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_NAMESPACE_INCOMPATIBLE)
        self.assertRegex(
            str(new_namespace_not_concatenate_with_db_error), "newNamespaceNotConcatenateWithDb")

        new_namespace_not_type_error = error_collection.get_error_by_command_name(
            "newNamespaceNotType")
        self.assertTrue(new_namespace_not_type_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_NAMESPACE_INCOMPATIBLE)
        self.assertRegex(str(new_namespace_not_type_error), "newNamespaceNotType")

        old_type_bson_any_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(old_type_bson_any_error), "oldTypeBsonAny")

        new_type_bson_any_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(new_type_bson_any_error), "newTypeBsonAny")

        new_type_not_enum_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_NOT_ENUM)
        self.assertRegex(str(new_type_not_enum_error), "newTypeNotEnum")

        new_type_not_struct_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_NOT_STRUCT)
        self.assertRegex(str(new_type_not_struct_error), "newTypeNotStruct")

        new_type_enum_or_struct_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_ENUM_OR_STRUCT)
        self.assertRegex(str(new_type_enum_or_struct_error), "newTypeEnumOrStruct")

        new_type_not_superset_error = error_collection.get_error_by_command_name(
            "newTypeNotSuperset")
        self.assertTrue(new_type_not_superset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_NOT_SUPERSET)
        self.assertRegex(str(new_type_not_superset_error), "newTypeNotSuperset")

        new_type_enum_not_superset_error = error_collection.get_error_by_command_name(
            "newTypeEnumNotSuperset")
        self.assertTrue(new_type_enum_not_superset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_NOT_SUPERSET)
        self.assertRegex(str(new_type_enum_not_superset_error), "newTypeEnumNotSuperset")

        new_type_struct_recursive_error = error_collection.get_error_by_command_name(
            "newTypeStructRecursive")
        self.assertTrue(new_type_struct_recursive_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_UNSTABLE)
        self.assertRegex(str(new_type_struct_recursive_error), "newTypeStructRecursive")

        new_type_field_unstable_error = error_collection.get_error_by_command_name(
            "newTypeFieldUnstable")
        self.assertTrue(new_type_field_unstable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_UNSTABLE)
        self.assertRegex(str(new_type_field_unstable_error), "newTypeFieldUnstable")

        new_type_field_required_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRED)
        self.assertRegex(str(new_type_field_required_error), "newTypeFieldRequired")

        new_type_field_missing_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_MISSING)
        self.assertRegex(str(new_type_field_missing_error), "newTypeFieldMissing")

        new_reply_field_variant_type_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE)
        self.assertRegex(str(new_reply_field_variant_type_error), "newReplyFieldVariantType")

        new_reply_field_variant_not_subset_error = error_collection.get_error_by_command_name(
            "newReplyFieldVariantNotSubset")
        self.assertTrue(new_reply_field_variant_not_subset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_variant_not_subset_error), "newReplyFieldVariantNotSubset")

        new_reply_field_variant_recursive_error = error_collection.get_error_by_command_name(
            "replyFieldVariantRecursive")
        self.assertTrue(new_reply_field_variant_recursive_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_NOT_SUBSET)
        self.assertRegex(str(new_reply_field_variant_recursive_error), "replyFieldVariantRecursive")

        new_reply_field_variant_struct_not_subset_error = error_collection.get_error_by_command_name(
            "newReplyFieldVariantStructNotSubset")
        self.assertTrue(new_reply_field_variant_struct_not_subset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_variant_struct_not_subset_error),
            "newReplyFieldVariantStructNotSubset")

        new_reply_field_variant_struct_recursive_error = error_collection.get_error_by_command_name(
            "replyFieldVariantStructRecursive")
        self.assertTrue(new_reply_field_variant_struct_recursive_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_variant_struct_recursive_error), "replyFieldVariantStructRecursive")

    def test_error_reply(self):
        """Tests the compatibility checker with the ErrorReply struct."""
        dir_path = path.dirname(path.realpath(__file__))

        self.assertFalse(
            idl_check_compatibility.check_error_reply(
                path.join(dir_path, "compatibility_test_pass/old/error_reply.idl"),
                path.join(dir_path, "compatibility_test_pass/new/error_reply.idl"),
                []).has_errors())

        error_collection_fail = idl_check_compatibility.check_error_reply(
            path.join(dir_path, "compatibility_test_fail/old/error_reply.idl"),
            path.join(dir_path, "compatibility_test_fail/new/error_reply.idl"), [])

        self.assertTrue(error_collection_fail.has_errors())
        self.assertTrue(error_collection_fail.count() == 1)

        new_error_reply_field_optional_error = error_collection_fail.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_OPTIONAL)
        self.assertRegex(str(new_error_reply_field_optional_error), "n/a")


if __name__ == '__main__':
    unittest.main()
