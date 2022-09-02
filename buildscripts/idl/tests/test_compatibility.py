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

import idl_check_compatibility  # noqa: E402 pylint: disable=wrong-import-position
import idl_compatibility_errors  # noqa: E402 pylint: disable=wrong-import-position


class TestIDLCompatibilityChecker(unittest.TestCase):
    """Test the IDL Compatibility Checker."""

    def test_should_pass(self):
        """Tests that compatible old and new IDL commands should pass."""
        dir_path = path.dirname(path.realpath(__file__))
        self.assertFalse(
            idl_check_compatibility.check_compatibility(
                path.join(dir_path, "compatibility_test_pass/old"),
                path.join(dir_path, "compatibility_test_pass/new"), ["src"], ["src"]).has_errors())

    def test_should_abort(self):
        """Tests that invalid old and new IDL commands should cause script to abort."""
        dir_path = path.dirname(path.realpath(__file__))
        # Test that when old command has a reply field with an invalid reply type, the script aborts.
        with self.assertRaises(SystemExit):
            idl_check_compatibility.check_compatibility(
                path.join(dir_path, "compatibility_test_fail/abort/invalid_reply_field_type"),
                path.join(dir_path, "compatibility_test_fail/abort/valid_reply_field_type"),
                ["src"], ["src"])

        # Test that when new command has a reply field with an invalid reply type, the script aborts.
        with self.assertRaises(SystemExit):
            idl_check_compatibility.check_compatibility(
                path.join(dir_path, "compatibility_test_fail/abort/valid_reply_field_type"),
                path.join(dir_path, "compatibility_test_fail/abort/invalid_reply_field_type"),
                ["src"], ["src"])

        # Test that when new command has a parameter with an invalid type, the script aborts.
        with self.assertRaises(SystemExit):
            idl_check_compatibility.check_compatibility(
                path.join(dir_path, "compatibility_test_fail/abort/invalid_command_parameter_type"),
                path.join(dir_path, "compatibility_test_fail/abort/valid_command_parameter_type"),
                ["src"], ["src"])

        # Test that when new command has a parameter with an invalid type, the script aborts.
        with self.assertRaises(SystemExit):
            idl_check_compatibility.check_compatibility(
                path.join(dir_path, "compatibility_test_fail/abort/valid_command_parameter_type"),
                path.join(dir_path, "compatibility_test_fail/abort/invalid_command_parameter_type"),
                ["src"], ["src"])

    # pylint: disable=invalid-name
    def test_newly_added_commands_should_fail(self):
        """Tests that incompatible newly added commands should fail."""
        dir_path = path.dirname(path.realpath(__file__))
        error_collection = idl_check_compatibility.check_compatibility(
            path.join(dir_path, "compatibility_test_fail/newly_added_commands"),
            path.join(dir_path, "compatibility_test_fail/newly_added_commands"), ["src"], ["src"])

        self.assertTrue(error_collection.has_errors())
        self.assertEqual(error_collection.count(), 6)

        new_parameter_no_unstable_field_error = error_collection.get_error_by_command_name(
            "newCommandParameterNoUnstableField")
        self.assertTrue(new_parameter_no_unstable_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_PARAMETER_REQUIRES_STABILITY)
        self.assertRegex(
            str(new_parameter_no_unstable_field_error), "newCommandParameterNoUnstableField")

        new_reply_no_unstable_field_error = error_collection.get_error_by_command_name(
            "newCommandReplyNoUnstableField")
        self.assertTrue(new_reply_no_unstable_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_REQUIRES_STABILITY)
        self.assertRegex(str(new_reply_no_unstable_field_error), "newCommandReplyNoUnstableField")

        new_command_type_struct_no_unstable_field_error = error_collection.get_error_by_command_name(
            "newCommandTypeStructFieldNoUnstableField")
        self.assertTrue(new_command_type_struct_no_unstable_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRES_STABILITY)
        self.assertRegex(
            str(new_command_type_struct_no_unstable_field_error),
            "newCommandTypeStructFieldNoUnstableField")

        new_parameter_bson_serialization_type_any_error = error_collection.get_error_by_command_name(
            "newCommandParameterBsonSerializationTypeAny")
        self.assertTrue(
            new_parameter_bson_serialization_type_any_error.error_id == idl_compatibility_errors.
            ERROR_ID_COMMAND_PARAMETER_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(new_parameter_bson_serialization_type_any_error),
            "newCommandParameterBsonSerializationTypeAny")

        new_reply_bson_serialization_type_any_error = error_collection.get_error_by_command_name(
            "newCommandReplyBsonSerializationTypeAny")
        self.assertTrue(
            new_reply_bson_serialization_type_any_error.error_id ==
            idl_compatibility_errors.ERROR_ID_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(new_reply_bson_serialization_type_any_error),
            "newCommandReplyBsonSerializationTypeAny")

        new_command_type_struct_bson_serialization_type_any_error = error_collection.get_error_by_command_name(
            "newCommandTypeStructFieldBsonSerializationTypeAny")
        self.assertTrue(
            new_command_type_struct_bson_serialization_type_any_error.error_id ==
            idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(new_command_type_struct_bson_serialization_type_any_error),
            "newCommandTypeStructFieldBsonSerializationTypeAny")

    # pylint: disable=invalid-name
    def test_should_fail(self):
        """Tests that incompatible old and new IDL commands should fail."""
        dir_path = path.dirname(path.realpath(__file__))
        error_collection = idl_check_compatibility.check_compatibility(
            path.join(dir_path, "compatibility_test_fail/old"),
            path.join(dir_path, "compatibility_test_fail/new"), ["src"], ["src"])

        self.assertTrue(error_collection.has_errors())

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

        strict_false_to_true_command_error = error_collection.get_error_by_command_name(
            "strictFalseToTrueCommand")
        self.assertTrue(strict_false_to_true_command_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_STRICT_TRUE_ERROR)
        self.assertRegex(str(strict_false_to_true_command_error), "strictFalseToTrueCommand")

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

        command_parameter_internal_error = error_collection.get_error_by_command_name(
            "commandParameterInternal")
        self.assertTrue(command_parameter_internal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_UNSTABLE)
        self.assertRegex(str(command_parameter_internal_error), "commandParameterInternal")

        command_parameter_stable_required_no_default_error = error_collection.get_error_by_command_name(
            "commandParameterStableRequiredNoDefault")
        self.assertTrue(
            command_parameter_stable_required_no_default_error.error_id ==
            idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_STABLE_REQUIRED_NO_DEFAULT)
        self.assertRegex(
            str(command_parameter_stable_required_no_default_error),
            "commandParameterStableRequiredNoDefault")

        command_parameter_required_error = error_collection.get_error_by_command_name(
            "commandParameterRequired")
        self.assertTrue(command_parameter_required_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_REQUIRED)
        self.assertRegex(str(command_parameter_required_error), "commandParameterRequired")

        old_command_parameter_type_bson_any_error = error_collection.get_error_by_command_name(
            "oldCommandParameterTypeBsonSerializationAny")
        self.assertTrue(
            old_command_parameter_type_bson_any_error.error_id == idl_compatibility_errors.
            ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(old_command_parameter_type_bson_any_error),
            "oldCommandParameterTypeBsonSerializationAny")

        new_command_parameter_type_bson_any_error = error_collection.get_error_by_command_name(
            "newCommandParameterTypeBsonSerializationAny")
        self.assertTrue(
            new_command_parameter_type_bson_any_error.error_id == idl_compatibility_errors.
            ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(new_command_parameter_type_bson_any_error),
            "newCommandParameterTypeBsonSerializationAny")

        old_param_type_bson_any_allow_list_error = error_collection.get_error_by_command_name(
            "oldParamTypeBsonAnyAllowList")
        self.assertTrue(
            old_param_type_bson_any_allow_list_error.error_id == idl_compatibility_errors.
            ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(old_param_type_bson_any_allow_list_error), "oldParamTypeBsonAnyAllowList")

        new_param_type_bson_any_allow_list_error = error_collection.get_error_by_command_name(
            "newParamTypeBsonAnyAllowList")
        self.assertTrue(
            new_param_type_bson_any_allow_list_error.error_id == idl_compatibility_errors.
            ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(new_param_type_bson_any_allow_list_error), "newParamTypeBsonAnyAllowList")

        command_parameter_type_bson_any_not_allowed_error = error_collection.get_error_by_command_name(
            "commandParameterTypeBsonSerializationAnyNotAllowed")
        self.assertTrue(
            command_parameter_type_bson_any_not_allowed_error.error_id == idl_compatibility_errors.
            ERROR_ID_COMMAND_PARAMETER_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(command_parameter_type_bson_any_not_allowed_error),
            "commandParameterTypeBsonSerializationAnyNotAllowed")

        command_parameter_cpp_type_not_equal_error = error_collection.get_error_by_command_name(
            "commandParameterCppTypeNotEqual")
        self.assertTrue(command_parameter_cpp_type_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_CPP_TYPE_NOT_EQUAL)
        self.assertRegex(
            str(command_parameter_cpp_type_not_equal_error), "commandParameterCppTypeNotEqual")

        command_parameter_serializer_not_equal_error = error_collection.get_error_by_command_name(
            "commandParameterSerializerNotEqual")
        self.assertEqual(command_parameter_serializer_not_equal_error.error_id,
                         idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_SERIALIZER_NOT_EQUAL)
        self.assertRegex(
            str(command_parameter_serializer_not_equal_error), "commandParameterSerializerNotEqual")

        command_parameter_deserializer_not_equal_error = error_collection.get_error_by_command_name(
            "commandParameterDeserializerNotEqual")
        self.assertTrue(command_parameter_deserializer_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_DESERIALIZER_NOT_EQUAL)
        self.assertRegex(
            str(command_parameter_deserializer_not_equal_error),
            "commandParameterDeserializerNotEqual")

        old_command_parameter_type_bson_any_unstable_error = error_collection.get_error_by_command_name(
            "oldCommandParamTypeBsonAnyUnstable")
        self.assertTrue(
            old_command_parameter_type_bson_any_unstable_error.error_id == idl_compatibility_errors.
            ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(old_command_parameter_type_bson_any_unstable_error),
            "oldCommandParamTypeBsonAnyUnstable")

        new_command_parameter_type_bson_any_unstable_error = error_collection.get_error_by_command_name(
            "newCommandParamTypeBsonAnyUnstable")
        self.assertTrue(
            new_command_parameter_type_bson_any_unstable_error.error_id == idl_compatibility_errors.
            ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(new_command_parameter_type_bson_any_unstable_error),
            "newCommandParamTypeBsonAnyUnstable")

        command_parameter_type_bson_any_not_allowed_unstable_error = error_collection.get_error_by_command_name(
            "commandParamTypeBsonAnyNotAllowedUnstable")
        self.assertTrue(command_parameter_type_bson_any_not_allowed_unstable_error.error_id ==
                        idl_compatibility_errors.
                        ERROR_ID_COMMAND_PARAMETER_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(command_parameter_type_bson_any_not_allowed_unstable_error),
            "commandParamTypeBsonAnyNotAllowedUnstable")

        command_parameter_cpp_type_not_equal_unstable_error = error_collection.get_error_by_command_name(
            "commandParameterCppTypeNotEqualUnstable")
        self.assertTrue(command_parameter_cpp_type_not_equal_unstable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_CPP_TYPE_NOT_EQUAL)
        self.assertRegex(
            str(command_parameter_cpp_type_not_equal_unstable_error),
            "commandParameterCppTypeNotEqualUnstable")

        parameter_field_type_bson_any_with_variant_unstable_error = error_collection.get_error_by_command_name_and_error_id(
            "parameterFieldTypeBsonAnyWithVariantUnstable", idl_compatibility_errors.
            ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(parameter_field_type_bson_any_with_variant_unstable_error.error_id ==
                        idl_compatibility_errors.
                        ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(parameter_field_type_bson_any_with_variant_unstable_error),
            "parameterFieldTypeBsonAnyWithVariantUnstable")

        parameter_field_type_bson_any_with_variant_unstable_error = error_collection.get_error_by_command_name_and_error_id(
            "parameterFieldTypeBsonAnyWithVariantUnstable", idl_compatibility_errors.
            ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(parameter_field_type_bson_any_with_variant_unstable_error.error_id ==
                        idl_compatibility_errors.
                        ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(parameter_field_type_bson_any_with_variant_unstable_error),
            "parameterFieldTypeBsonAnyWithVariantUnstable")

        newly_added_param_bson_any_not_allowed_error = error_collection.get_error_by_command_name(
            "newlyAddedParamBsonAnyNotAllowed")
        self.assertTrue(
            newly_added_param_bson_any_not_allowed_error.error_id == idl_compatibility_errors.
            ERROR_ID_COMMAND_PARAMETER_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(newly_added_param_bson_any_not_allowed_error), "newlyAddedParamBsonAnyNotAllowed")

        new_command_parameter_type_enum_not_superset = error_collection.get_error_by_command_name(
            "newCommandParameterTypeEnumNotSuperset")
        self.assertTrue(new_command_parameter_type_enum_not_superset.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_parameter_type_enum_not_superset),
            "newCommandParameterTypeEnumNotSuperset")

        new_command_parameter_type_not_enum = error_collection.get_error_by_command_name(
            "newCommandParameterTypeNotEnum")
        self.assertTrue(new_command_parameter_type_not_enum.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_NOT_ENUM)
        self.assertRegex(str(new_command_parameter_type_not_enum), "newCommandParameterTypeNotEnum")

        new_command_parameter_type_not_struct = error_collection.get_error_by_command_name(
            "newCommandParameterTypeNotStruct")
        self.assertTrue(new_command_parameter_type_not_struct.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_NOT_STRUCT)
        self.assertRegex(
            str(new_command_parameter_type_not_struct), "newCommandParameterTypeNotStruct")

        new_command_parameter_type_enum_or_struct_one = error_collection.get_error_by_command_name(
            "newCommandParameterTypeEnumOrStructOne")
        self.assertTrue(new_command_parameter_type_enum_or_struct_one.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_ENUM_OR_STRUCT)
        self.assertRegex(
            str(new_command_parameter_type_enum_or_struct_one),
            "newCommandParameterTypeEnumOrStructOne")

        new_command_parameter_type_enum_or_struct_two = error_collection.get_error_by_command_name(
            "newCommandParameterTypeEnumOrStructTwo")
        self.assertTrue(new_command_parameter_type_enum_or_struct_two.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_ENUM_OR_STRUCT)
        self.assertRegex(
            str(new_command_parameter_type_enum_or_struct_two),
            "newCommandParameterTypeEnumOrStructTwo")

        new_command_parameter_type_bson_not_superset = error_collection.get_error_by_command_name(
            "newCommandParameterTypeBsonNotSuperset")
        self.assertTrue(new_command_parameter_type_bson_not_superset.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_parameter_type_bson_not_superset),
            "newCommandParameterTypeBsonNotSuperset")

        new_command_parameter_type_recursive_one_error = error_collection.get_error_by_command_name(
            "newCommandParameterTypeStructRecursiveOne")
        self.assertTrue(new_command_parameter_type_recursive_one_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_UNSTABLE)
        self.assertRegex(
            str(new_command_parameter_type_recursive_one_error),
            "newCommandParameterTypeStructRecursiveOne")

        new_command_parameter_type_recursive_two_error = error_collection.get_error_by_command_name(
            "newCommandParameterTypeStructRecursiveTwo")
        self.assertTrue(new_command_parameter_type_recursive_two_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_parameter_type_recursive_two_error),
            "newCommandParameterTypeStructRecursiveTwo")

        new_reply_field_unstable_error = error_collection.get_error_by_command_name(
            "newReplyFieldUnstable")
        self.assertTrue(new_reply_field_unstable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_UNSTABLE)
        self.assertRegex(str(new_reply_field_unstable_error), "newReplyFieldUnstable")

        new_reply_field_internal_error = error_collection.get_error_by_command_name(
            "newReplyFieldInternal")
        self.assertTrue(new_reply_field_internal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_UNSTABLE)
        self.assertRegex(str(new_reply_field_internal_error), "newReplyFieldInternal")

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
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_NOT_SUBSET)
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
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_type_bson_not_subset_error), "newReplyFieldTypeBsonNotSubset")

        new_reply_field_type_bson_not_subset_two_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeBsonNotSubsetTwo")
        self.assertTrue(new_reply_field_type_bson_not_subset_two_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_type_bson_not_subset_two_error),
            "newReplyFieldTypeBsonNotSubsetTwo")

        old_reply_field_type_bson_any_error = error_collection.get_error_by_command_name(
            "oldReplyFieldTypeBsonAny")
        self.assertTrue(old_reply_field_type_bson_any_error.error_id == idl_compatibility_errors.
                        ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(old_reply_field_type_bson_any_error), "oldReplyFieldTypeBsonAny")

        new_reply_field_type_bson_any_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeBsonAny")
        self.assertTrue(new_reply_field_type_bson_any_error.error_id == idl_compatibility_errors.
                        ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(new_reply_field_type_bson_any_error), "newReplyFieldTypeBsonAny")

        old_reply_field_type_bson_any_allow_list_error = error_collection.get_error_by_command_name(
            "oldReplyFieldTypeBsonAnyAllowList")
        self.assertTrue(
            old_reply_field_type_bson_any_allow_list_error.error_id ==
            idl_compatibility_errors.ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(old_reply_field_type_bson_any_allow_list_error),
            "oldReplyFieldTypeBsonAnyAllowList")

        new_reply_field_type_bson_any_allow_list_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeBsonAnyAllowList")
        self.assertTrue(
            new_reply_field_type_bson_any_allow_list_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(new_reply_field_type_bson_any_allow_list_error),
            "newReplyFieldTypeBsonAnyAllowList")

        reply_field_type_bson_any_not_allowed_error = error_collection.get_error_by_command_name(
            "replyFieldTypeBsonAnyNotAllowed")
        self.assertTrue(
            reply_field_type_bson_any_not_allowed_error.error_id ==
            idl_compatibility_errors.ERROR_ID_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(reply_field_type_bson_any_not_allowed_error), "replyFieldTypeBsonAnyNotAllowed")

        reply_field_type_bson_any_with_variant_error = error_collection.get_error_by_command_name_and_error_id(
            "replyFieldTypeBsonAnyWithVariant",
            idl_compatibility_errors.ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            reply_field_type_bson_any_with_variant_error.error_id ==
            idl_compatibility_errors.ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(reply_field_type_bson_any_with_variant_error), "replyFieldTypeBsonAnyWithVariant")

        reply_field_type_bson_any_with_variant_error = error_collection.get_error_by_command_name_and_error_id(
            "replyFieldTypeBsonAnyWithVariant",
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            reply_field_type_bson_any_with_variant_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(reply_field_type_bson_any_with_variant_error), "replyFieldTypeBsonAnyWithVariant")

        old_reply_field_type_bson_any_unstable_error = error_collection.get_error_by_command_name(
            "oldReplyFieldTypeBsonAnyUnstable")
        self.assertTrue(
            old_reply_field_type_bson_any_unstable_error.error_id ==
            idl_compatibility_errors.ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(old_reply_field_type_bson_any_unstable_error), "oldReplyFieldTypeBsonAnyUnstable")

        new_reply_field_type_bson_any_unstable_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeBsonAnyUnstable")
        self.assertTrue(
            new_reply_field_type_bson_any_unstable_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(new_reply_field_type_bson_any_unstable_error), "newReplyFieldTypeBsonAnyUnstable")

        reply_field_type_bson_any_not_allowed_unstable_error = error_collection.get_error_by_command_name(
            "replyFieldTypeBsonAnyNotAllowedUnstable")
        self.assertTrue(
            reply_field_type_bson_any_not_allowed_unstable_error.error_id ==
            idl_compatibility_errors.ERROR_ID_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(reply_field_type_bson_any_not_allowed_unstable_error),
            "replyFieldTypeBsonAnyNotAllowedUnstable")

        reply_field_type_bson_any_with_variant_unstable_error = error_collection.get_error_by_command_name_and_error_id(
            "replyFieldTypeBsonAnyWithVariantUnstable",
            idl_compatibility_errors.ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            reply_field_type_bson_any_with_variant_unstable_error.error_id ==
            idl_compatibility_errors.ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(reply_field_type_bson_any_with_variant_unstable_error),
            "replyFieldTypeBsonAnyWithVariantUnstable")

        reply_field_type_bson_any_with_variant_unstable_error = error_collection.get_error_by_command_name_and_error_id(
            "replyFieldTypeBsonAnyWithVariantUnstable",
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            reply_field_type_bson_any_with_variant_unstable_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(reply_field_type_bson_any_with_variant_unstable_error),
            "replyFieldTypeBsonAnyWithVariantUnstable")

        reply_field_cpp_type_not_equal_unstable_error = error_collection.get_error_by_command_name(
            "replyFieldCppTypeNotEqualUnstable")
        self.assertTrue(reply_field_cpp_type_not_equal_unstable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_CPP_TYPE_NOT_EQUAL)
        self.assertRegex(
            str(reply_field_cpp_type_not_equal_unstable_error), "replyFieldCppTypeNotEqualUnstable")

        newly_added_reply_field_bson_any_not_allowed_error = error_collection.get_error_by_command_name(
            "newlyAddedReplyFieldTypeBsonAnyNotAllowed")
        self.assertTrue(
            newly_added_reply_field_bson_any_not_allowed_error.error_id ==
            idl_compatibility_errors.ERROR_ID_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(newly_added_reply_field_bson_any_not_allowed_error),
            "newlyAddedReplyFieldTypeBsonAnyNotAllowed")

        reply_field_type_bson_any_with_variant_with_array_error = error_collection.get_error_by_command_name_and_error_id(
            "replyFieldTypeBsonAnyWithVariantWithArray",
            idl_compatibility_errors.ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            reply_field_type_bson_any_with_variant_with_array_error.error_id ==
            idl_compatibility_errors.ERROR_ID_OLD_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(reply_field_type_bson_any_with_variant_with_array_error),
            "replyFieldTypeBsonAnyWithVariantWithArray")

        reply_field_type_bson_any_with_variant_with_array_error = error_collection.get_error_by_command_name_and_error_id(
            "replyFieldTypeBsonAnyWithVariantWithArray",
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            reply_field_type_bson_any_with_variant_with_array_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(reply_field_type_bson_any_with_variant_with_array_error),
            "replyFieldTypeBsonAnyWithVariantWithArray")

        parameter_field_type_bson_any_with_variant_error = error_collection.get_error_by_command_name_and_error_id(
            "parameterFieldTypeBsonAnyWithVariant", idl_compatibility_errors.
            ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            parameter_field_type_bson_any_with_variant_error.error_id == idl_compatibility_errors.
            ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(parameter_field_type_bson_any_with_variant_error),
            "parameterFieldTypeBsonAnyWithVariant")

        parameter_field_type_bson_any_with_variant_error = error_collection.get_error_by_command_name_and_error_id(
            "parameterFieldTypeBsonAnyWithVariant", idl_compatibility_errors.
            ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            parameter_field_type_bson_any_with_variant_error.error_id == idl_compatibility_errors.
            ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(parameter_field_type_bson_any_with_variant_error),
            "parameterFieldTypeBsonAnyWithVariant")

        parameter_field_type_bson_any_with_variant_with_array_error = error_collection.get_error_by_command_name_and_error_id(
            "parameterFieldTypeBsonAnyWithVariantWithArray", idl_compatibility_errors.
            ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(parameter_field_type_bson_any_with_variant_with_array_error.error_id ==
                        idl_compatibility_errors.
                        ERROR_ID_OLD_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(parameter_field_type_bson_any_with_variant_with_array_error),
            "parameterFieldTypeBsonAnyWithVariantWithArray")

        parameter_field_type_bson_any_with_variant_with_array_error = error_collection.get_error_by_command_name_and_error_id(
            "parameterFieldTypeBsonAnyWithVariantWithArray", idl_compatibility_errors.
            ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(parameter_field_type_bson_any_with_variant_with_array_error.error_id ==
                        idl_compatibility_errors.
                        ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(parameter_field_type_bson_any_with_variant_with_array_error),
            "parameterFieldTypeBsonAnyWithVariantWithArray")

        command_type_bson_any_with_variant_error = error_collection.get_error_by_command_name_and_error_id(
            "commandTypeBsonAnyWithVariant",
            idl_compatibility_errors.ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            command_type_bson_any_with_variant_error.error_id ==
            idl_compatibility_errors.ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(command_type_bson_any_with_variant_error), "commandTypeBsonAnyWithVariant")

        command_type_bson_any_with_variant_error = error_collection.get_error_by_command_name_and_error_id(
            "commandTypeBsonAnyWithVariant",
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            command_type_bson_any_with_variant_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(command_type_bson_any_with_variant_error), "commandTypeBsonAnyWithVariant")

        command_type_bson_any_with_variant_with_array_error = error_collection.get_error_by_command_name_and_error_id(
            "commandTypeBsonAnyWithVariantWithArray",
            idl_compatibility_errors.ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            command_type_bson_any_with_variant_with_array_error.error_id ==
            idl_compatibility_errors.ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(command_type_bson_any_with_variant_with_array_error),
            "commandTypeBsonAnyWithVariantWithArray")

        command_type_bson_any_with_variant_with_array_error = error_collection.get_error_by_command_name_and_error_id(
            "commandTypeBsonAnyWithVariantWithArray",
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            command_type_bson_any_with_variant_with_array_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(command_type_bson_any_with_variant_with_array_error),
            "commandTypeBsonAnyWithVariantWithArray")

        reply_field_cpp_type_not_equal_error = error_collection.get_error_by_command_name(
            "replyFieldCppTypeNotEqual")
        self.assertTrue(reply_field_cpp_type_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_CPP_TYPE_NOT_EQUAL)
        self.assertRegex(str(reply_field_cpp_type_not_equal_error), "replyFieldCppTypeNotEqual")

        reply_field_serializer_not_equal_error = error_collection.get_error_by_command_name(
            "replyFieldSerializerNotEqual")
        self.assertTrue(reply_field_serializer_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_SERIALIZER_NOT_EQUAL)
        self.assertRegex(
            str(reply_field_serializer_not_equal_error), "replyFieldSerializerNotEqual")

        reply_field_deserializer_not_equal_error = error_collection.get_error_by_command_name(
            "replyFieldDeserializerNotEqual")
        self.assertTrue(reply_field_deserializer_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_DESERIALIZER_NOT_EQUAL)
        self.assertRegex(
            str(reply_field_deserializer_not_equal_error), "replyFieldDeserializerNotEqual")

        new_reply_field_type_struct_one_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeStructRecursiveOne")
        self.assertTrue(new_reply_field_type_struct_one_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_UNSTABLE)
        self.assertRegex(
            str(new_reply_field_type_struct_one_error), "newReplyFieldTypeStructRecursiveOne")

        new_reply_field_type_struct_two_error = error_collection.get_error_by_command_name(
            "newReplyFieldTypeStructRecursiveTwo")
        self.assertTrue(new_reply_field_type_struct_two_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_NOT_SUBSET)
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

        old_type_bson_any_error = error_collection.get_error_by_command_name("oldTypeBsonAny")
        self.assertTrue(old_type_bson_any_error.error_id == idl_compatibility_errors.
                        ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(old_type_bson_any_error), "oldTypeBsonAny")

        new_type_bson_any_error = error_collection.get_error_by_command_name("newTypeBsonAny")
        self.assertTrue(new_type_bson_any_error.error_id == idl_compatibility_errors.
                        ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(new_type_bson_any_error), "newTypeBsonAny")

        old_type_bson_any_allow_list_error = error_collection.get_error_by_command_name(
            "oldTypeBsonAnyAllowList")
        self.assertTrue(old_type_bson_any_allow_list_error.error_id == idl_compatibility_errors.
                        ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(old_type_bson_any_allow_list_error), "oldTypeBsonAnyAllowList")

        new_type_bson_any_allow_list_error = error_collection.get_error_by_command_name(
            "newTypeBsonAnyAllowList")
        self.assertTrue(new_type_bson_any_allow_list_error.error_id == idl_compatibility_errors.
                        ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(new_type_bson_any_allow_list_error), "newTypeBsonAnyAllowList")

        type_bson_any_not_allowed_error = error_collection.get_error_by_command_name(
            "typeBsonAnyNotAllowed")
        self.assertTrue(type_bson_any_not_allowed_error.error_id == idl_compatibility_errors.
                        ERROR_ID_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(str(type_bson_any_not_allowed_error), "typeBsonAnyNotAllowed")

        command_cpp_type_not_equal_error = error_collection.get_error_by_command_name(
            "commandCppTypeNotEqual")
        self.assertTrue(command_cpp_type_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_CPP_TYPE_NOT_EQUAL)
        self.assertRegex(str(command_cpp_type_not_equal_error), "commandCppTypeNotEqual")

        command_serializer_not_equal_error = error_collection.get_error_by_command_name(
            "commandSerializerNotEqual")
        self.assertTrue(command_serializer_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_SERIALIZER_NOT_EQUAL)
        self.assertRegex(str(command_serializer_not_equal_error), "commandSerializerNotEqual")

        command_deserializer_not_equal_error = error_collection.get_error_by_command_name(
            "commandDeserializerNotEqual")
        self.assertTrue(command_deserializer_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_DESERIALIZER_NOT_EQUAL)
        self.assertRegex(str(command_deserializer_not_equal_error), "commandDeserializerNotEqual")

        old_type_bson_any_unstable_error = error_collection.get_error_by_command_name(
            "oldTypeBsonAnyUnstable")
        self.assertTrue(old_type_bson_any_unstable_error.error_id == idl_compatibility_errors.
                        ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(old_type_bson_any_unstable_error), "oldTypeBsonAnyUnstable")

        new_type_bson_any_unstable_error = error_collection.get_error_by_command_name(
            "newTypeBsonAnyUnstable")
        self.assertTrue(new_type_bson_any_unstable_error.error_id == idl_compatibility_errors.
                        ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(str(new_type_bson_any_unstable_error), "newTypeBsonAnyUnstable")

        type_bson_any_not_allowed_unstable_error = error_collection.get_error_by_command_name(
            "typeBsonAnyNotAllowedUnstable")
        self.assertTrue(
            type_bson_any_not_allowed_unstable_error.error_id ==
            idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(type_bson_any_not_allowed_unstable_error), "typeBsonAnyNotAllowedUnstable")

        command_cpp_type_not_equal_unstable_error = error_collection.get_error_by_command_name(
            "commandCppTypeNotEqualUnstable")
        self.assertTrue(command_cpp_type_not_equal_unstable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_CPP_TYPE_NOT_EQUAL)
        self.assertRegex(
            str(command_cpp_type_not_equal_unstable_error), "commandCppTypeNotEqualUnstable")

        command_type_bson_any_with_variant_unstable_error = error_collection.get_error_by_command_name_and_error_id(
            "commandTypeBsonAnyWithVariantUnstable",
            idl_compatibility_errors.ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            command_type_bson_any_with_variant_unstable_error.error_id ==
            idl_compatibility_errors.ERROR_ID_OLD_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(command_type_bson_any_with_variant_unstable_error),
            "commandTypeBsonAnyWithVariantUnstable")

        command_type_bson_any_with_variant_unstable_error = error_collection.get_error_by_command_name_and_error_id(
            "commandTypeBsonAnyWithVariantUnstable",
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertTrue(
            command_type_bson_any_with_variant_unstable_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(command_type_bson_any_with_variant_unstable_error),
            "commandTypeBsonAnyWithVariantUnstable")

        newly_added_type_field_bson_any_not_allowed_error = error_collection.get_error_by_command_name(
            "newlyAddedTypeFieldBsonAnyNotAllowed")
        self.assertTrue(
            newly_added_type_field_bson_any_not_allowed_error.error_id ==
            idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY_NOT_ALLOWED)
        self.assertRegex(
            str(newly_added_type_field_bson_any_not_allowed_error),
            "newlyAddedTypeFieldBsonAnyNotAllowed")

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

        new_type_field_added_required_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_ADDED_REQUIRED)
        self.assertRegex(str(new_type_field_added_required_error), "newTypeFieldAddedRequired")

        new_type_field_stable_required_no_default_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_STABLE_REQUIRED_NO_DEFAULT)
        self.assertRegex(
            str(new_type_field_stable_required_no_default_error),
            "newTypeFieldStableRequiredNoDefault")

        new_reply_field_variant_type_error = error_collection.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE)
        self.assertRegex(str(new_reply_field_variant_type_error), "newReplyFieldVariantType")

        new_reply_field_variant_not_subset_error = error_collection.get_error_by_command_name(
            "newReplyFieldVariantNotSubset")
        self.assertTrue(new_reply_field_variant_not_subset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_variant_not_subset_error), "newReplyFieldVariantNotSubset")

        new_reply_field_variant_not_subset_two_errors = error_collection.get_all_errors_by_command_name(
            "newReplyFieldVariantNotSubsetTwo")
        self.assertTrue(len(new_reply_field_variant_not_subset_two_errors) == 2)
        for error in new_reply_field_variant_not_subset_two_errors:
            self.assertTrue(error.error_id == idl_compatibility_errors.
                            ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET)

        new_reply_field_variant_recursive_error = error_collection.get_error_by_command_name(
            "replyFieldVariantRecursive")
        self.assertTrue(new_reply_field_variant_recursive_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_NOT_SUBSET)
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
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_variant_struct_recursive_error), "replyFieldVariantStructRecursive")

        new_reply_field_variant_not_subset_with_array_error = error_collection.get_error_by_command_name(
            "newReplyFieldVariantNotSubsetWithArray")
        self.assertTrue(new_reply_field_variant_not_subset_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_variant_not_subset_with_array_error),
            "newReplyFieldVariantNotSubsetWithArray")

        new_reply_field_variant_not_subset_with_array_two_errors = error_collection.get_all_errors_by_command_name(
            "newReplyFieldVariantNotSubsetTwoWithArray")
        self.assertTrue(len(new_reply_field_variant_not_subset_with_array_two_errors) == 2)
        for error in new_reply_field_variant_not_subset_with_array_two_errors:
            self.assertTrue(error.error_id == idl_compatibility_errors.
                            ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET)

        new_reply_field_variant_recursive_with_array_error = error_collection.get_error_by_command_name(
            "replyFieldVariantRecursiveWithArray")
        self.assertTrue(new_reply_field_variant_recursive_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_variant_recursive_with_array_error),
            "replyFieldVariantRecursiveWithArray")

        new_reply_field_variant_struct_not_subset_with_array_error = error_collection.get_error_by_command_name(
            "newReplyFieldVariantStructNotSubsetWithArray")
        self.assertTrue(new_reply_field_variant_struct_not_subset_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_variant_struct_not_subset_with_array_error),
            "newReplyFieldVariantStructNotSubsetWithArray")

        new_reply_field_variant_struct_recursive_with_array_error = error_collection.get_error_by_command_name(
            "replyFieldVariantStructRecursiveWithArray")
        self.assertTrue(new_reply_field_variant_struct_recursive_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_NOT_SUBSET)
        self.assertRegex(
            str(new_reply_field_variant_struct_recursive_with_array_error),
            "replyFieldVariantStructRecursiveWithArray")

        new_command_parameter_contains_validator_error = error_collection.get_error_by_command_name(
            "newCommandParameterValidator")
        self.assertTrue(new_command_parameter_contains_validator_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_CONTAINS_VALIDATOR)
        self.assertRegex(
            str(new_command_parameter_contains_validator_error), "newCommandParameterValidator")

        command_parameter_validators_not_equal_error = error_collection.get_error_by_command_name(
            "commandParameterValidatorsNotEqual")
        self.assertTrue(command_parameter_validators_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_VALIDATORS_NOT_EQUAL)
        self.assertRegex(
            str(command_parameter_validators_not_equal_error), "commandParameterValidatorsNotEqual")

        new_command_type_contains_validator_error = error_collection.get_error_by_command_name(
            "newCommandTypeValidator")
        self.assertTrue(new_command_type_contains_validator_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_CONTAINS_VALIDATOR)
        self.assertRegex(str(new_command_type_contains_validator_error), "newCommandTypeValidator")

        command_type_validators_not_equal_error = error_collection.get_error_by_command_name(
            "commandTypeValidatorsNotEqual")
        self.assertTrue(command_type_validators_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_VALIDATORS_NOT_EQUAL)
        self.assertRegex(
            str(command_type_validators_not_equal_error), "commandTypeValidatorsNotEqual")
        array_command_type_error = error_collection.get_error_by_command_name(
            "arrayCommandTypeError")
        self.assertTrue(array_command_type_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_NOT_STRUCT)
        self.assertRegex(str(array_command_type_error), "ArrayTypeStruct")
        array_command_param_type_two_errors = error_collection.get_all_errors_by_command_name(
            "arrayCommandParameterTypeError")
        self.assertTrue(len(array_command_param_type_two_errors) == 2)
        self.assertTrue(array_command_param_type_two_errors[0].error_id ==
                        idl_compatibility_errors.ERROR_ID_REMOVED_COMMAND_PARAMETER)
        self.assertRegex(str(array_command_param_type_two_errors[0]), "ArrayCommandParameter")
        self.assertTrue(array_command_param_type_two_errors[1].error_id ==
                        idl_compatibility_errors.ERROR_ID_ADDED_REQUIRED_COMMAND_PARAMETER)
        self.assertRegex(str(array_command_param_type_two_errors[1]), "fieldOne")

        new_param_variant_not_superset_error = error_collection.get_error_by_command_name(
            "newParamVariantNotSuperset")
        self.assertTrue(new_param_variant_not_superset_error.error_id == idl_compatibility_errors.
                        ERROR_ID_NEW_COMMAND_PARAMETER_VARIANT_TYPE_NOT_SUPERSET)
        self.assertRegex(str(new_param_variant_not_superset_error), "newParamVariantNotSuperset")

        new_param_variant_not_superset_two_errors = error_collection.get_all_errors_by_command_name(
            "newParamVariantNotSupersetTwo")
        self.assertTrue(len(new_param_variant_not_superset_two_errors) == 2)
        for error in new_param_variant_not_superset_two_errors:
            self.assertTrue(error.error_id == idl_compatibility_errors.
                            ERROR_ID_NEW_COMMAND_PARAMETER_VARIANT_TYPE_NOT_SUPERSET)

        new_param_type_not_variant_error = error_collection.get_error_by_command_name(
            "newParamTypeNotVariant")
        self.assertTrue(new_param_type_not_variant_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAMETER_TYPE_NOT_VARIANT)
        self.assertRegex(str(new_param_type_not_variant_error), "newParamTypeNotVariant")

        new_param_variant_recursive_error = error_collection.get_error_by_command_name(
            "newParamVariantRecursive")
        self.assertTrue(new_param_variant_recursive_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET)
        self.assertRegex(str(new_param_variant_recursive_error), "newParamVariantRecursive")

        new_param_variant_struct_not_superset_error = error_collection.get_error_by_command_name(
            "newParamVariantStructNotSuperset")
        self.assertTrue(
            new_param_variant_struct_not_superset_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAMETER_VARIANT_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_param_variant_struct_not_superset_error), "newParamVariantStructNotSuperset")

        new_param_variant_struct_recursive_error = error_collection.get_error_by_command_name(
            "newParamVariantStructRecursive")
        self.assertTrue(new_param_variant_struct_recursive_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_param_variant_struct_recursive_error), "newParamVariantStructRecursive")

        new_command_type_variant_not_superset_error = error_collection.get_error_by_command_name(
            "newCommandTypeVariantNotSuperset")
        self.assertTrue(new_command_type_variant_not_superset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_VARIANT_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_type_variant_not_superset_error), "newCommandTypeVariantNotSuperset")

        new_command_type_variant_not_superset_two_errors = error_collection.get_all_errors_by_command_name(
            "newCommandTypeVariantNotSupersetTwo")
        self.assertTrue(len(new_command_type_variant_not_superset_two_errors) == 2)
        for error in new_command_type_variant_not_superset_two_errors:
            self.assertTrue(error.error_id ==
                            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_VARIANT_TYPE_NOT_SUPERSET)

        new_command_type_not_variant_error = error_collection.get_error_by_command_name(
            "newCommandTypeNotVariant")
        self.assertTrue(new_command_type_not_variant_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_NOT_VARIANT)
        self.assertRegex(str(new_command_type_not_variant_error), "newCommandTypeNotVariant")

        new_command_type_variant_recursive_error = error_collection.get_error_by_command_name(
            "newCommandTypeVariantRecursive")
        self.assertTrue(new_command_type_variant_recursive_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_type_variant_recursive_error), "newCommandTypeVariantRecursive")

        new_command_type_variant_struct_not_superset_error = error_collection.get_error_by_command_name(
            "newCommandTypeVariantStructNotSuperset")
        self.assertTrue(new_command_type_variant_struct_not_superset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_VARIANT_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_type_variant_struct_not_superset_error),
            "newCommandTypeVariantStructNotSuperset")

        new_command_type_variant_struct_recursive_error = error_collection.get_error_by_command_name(
            "newCommandTypeVariantStructRecursive")
        self.assertTrue(new_command_type_variant_struct_recursive_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_type_variant_struct_recursive_error),
            "newCommandTypeVariantStructRecursive")
        new_reply_field_contains_validator_error = error_collection.get_error_by_command_name(
            "newReplyFieldValidator")
        self.assertTrue(new_reply_field_contains_validator_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_CONTAINS_VALIDATOR)
        self.assertRegex(str(new_reply_field_contains_validator_error), "newReplyFieldValidator")

        reply_field_validators_not_equal_error = error_collection.get_error_by_command_name(
            "replyFieldValidatorsNotEqual")
        self.assertTrue(reply_field_validators_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REPLY_FIELD_VALIDATORS_NOT_EQUAL)
        self.assertRegex(
            str(reply_field_validators_not_equal_error), "replyFieldValidatorsNotEqual")

        simple_check_not_equal_error = error_collection.get_error_by_command_name(
            "simpleCheckNotEqual")
        self.assertTrue(simple_check_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_CHECK_NOT_EQUAL)
        self.assertRegex(str(simple_check_not_equal_error), "simpleCheckNotEqual")

        simple_check_not_equal_error_two = error_collection.get_error_by_command_name(
            "simpleCheckNotEqualTwo")
        self.assertTrue(simple_check_not_equal_error_two.error_id ==
                        idl_compatibility_errors.ERROR_ID_CHECK_NOT_EQUAL)
        self.assertRegex(str(simple_check_not_equal_error_two), "simpleCheckNotEqualTwo")

        simple_check_not_equal_error_three = error_collection.get_error_by_command_name(
            "simpleCheckNotEqualThree")
        self.assertTrue(simple_check_not_equal_error_three.error_id ==
                        idl_compatibility_errors.ERROR_ID_CHECK_NOT_EQUAL)
        self.assertRegex(str(simple_check_not_equal_error_three), "simpleCheckNotEqualThree")

        simple_resource_pattern_not_equal_error = error_collection.get_error_by_command_name(
            "simpleResourcePatternNotEqual")
        self.assertTrue(simple_resource_pattern_not_equal_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_RESOURCE_PATTERN_NOT_EQUAL)
        self.assertRegex(
            str(simple_resource_pattern_not_equal_error), "simpleResourcePatternNotEqual")

        new_simple_action_types_not_subset_error = error_collection.get_error_by_command_name(
            "newSimpleActionTypesNotSubset")
        self.assertTrue(new_simple_action_types_not_subset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_ACTION_TYPES_NOT_SUBSET)
        self.assertRegex(
            str(new_simple_action_types_not_subset_error), "newSimpleActionTypesNotSubset")

        new_param_variant_not_superset_with_array_error = error_collection.get_error_by_command_name(
            "newParamVariantNotSupersetWithArray")
        self.assertTrue(
            new_param_variant_not_superset_with_array_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAMETER_VARIANT_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_param_variant_not_superset_with_array_error),
            "newParamVariantNotSupersetWithArray")

        new_param_variant_not_superset_with_array_two_errors = error_collection.get_all_errors_by_command_name(
            "newParamVariantNotSupersetTwoWithArray")
        self.assertTrue(len(new_param_variant_not_superset_with_array_two_errors) == 2)
        for error in new_param_variant_not_superset_with_array_two_errors:
            self.assertTrue(error.error_id == idl_compatibility_errors.
                            ERROR_ID_NEW_COMMAND_PARAMETER_VARIANT_TYPE_NOT_SUPERSET)

        new_param_variant_recursive_with_array_error = error_collection.get_error_by_command_name(
            "newParamVariantRecursiveWithArray")
        self.assertTrue(new_param_variant_recursive_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_param_variant_recursive_with_array_error), "newParamVariantRecursiveWithArray")

        new_param_variant_struct_not_superset_with_array_error = error_collection.get_error_by_command_name(
            "newParamVariantStructNotSupersetWithArray")
        self.assertTrue(
            new_param_variant_struct_not_superset_with_array_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAMETER_VARIANT_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_param_variant_struct_not_superset_with_array_error),
            "newParamVariantStructNotSupersetWithArray")

        new_param_variant_struct_recursive_with_array_error = error_collection.get_error_by_command_name(
            "newParamVariantStructRecursiveWithArray")
        self.assertTrue(new_param_variant_struct_recursive_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_param_variant_struct_recursive_with_array_error),
            "newParamVariantStructRecursiveWithArray")

        new_command_type_variant_not_superset_with_array_error = error_collection.get_error_by_command_name(
            "newCommandTypeVariantNotSupersetWithArray")
        self.assertTrue(new_command_type_variant_not_superset_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_VARIANT_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_type_variant_not_superset_with_array_error),
            "newCommandTypeVariantNotSupersetWithArray")

        new_command_type_variant_not_superset_with_array_two_errors = error_collection.get_all_errors_by_command_name(
            "newCommandTypeVariantNotSupersetTwoWithArray")
        self.assertTrue(len(new_command_type_variant_not_superset_with_array_two_errors) == 2)
        for error in new_command_type_variant_not_superset_with_array_two_errors:
            self.assertTrue(error.error_id ==
                            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_VARIANT_TYPE_NOT_SUPERSET)

        new_command_type_variant_recursive_with_array_error = error_collection.get_error_by_command_name(
            "newCommandTypeVariantRecursiveWithArray")
        self.assertTrue(new_command_type_variant_recursive_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_type_variant_recursive_with_array_error),
            "newCommandTypeVariantRecursiveWithArray")

        new_command_type_variant_struct_not_superset_with_array_error = error_collection.get_error_by_command_name(
            "newCommandTypeVariantStructNotSupersetWithArray")
        self.assertTrue(new_command_type_variant_struct_not_superset_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_VARIANT_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_type_variant_struct_not_superset_with_array_error),
            "newCommandTypeVariantStructNotSupersetWithArray")

        new_command_type_variant_struct_recursive_with_array_error = error_collection.get_error_by_command_name(
            "newCommandTypeVariantStructRecursiveWithArray")
        self.assertTrue(new_command_type_variant_struct_recursive_with_array_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_command_type_variant_struct_recursive_with_array_error),
            "newCommandTypeVariantStructRecursiveWithArray")

        access_check_type_change_error = error_collection.get_error_by_command_name(
            "accessCheckTypeChange")
        self.assertTrue(access_check_type_change_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_ACCESS_CHECK_TYPE_NOT_EQUAL)
        self.assertRegex(str(access_check_type_change_error), "accessCheckTypeChange")

        access_check_type_change_two_error = error_collection.get_error_by_command_name(
            "accessCheckTypeChangeTwo")
        self.assertTrue(access_check_type_change_two_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_ACCESS_CHECK_TYPE_NOT_EQUAL)
        self.assertRegex(str(access_check_type_change_two_error), "accessCheckTypeChangeTwo")

        complex_checks_not_subset_error = error_collection.get_error_by_command_name(
            "complexChecksNotSubset")
        self.assertTrue(complex_checks_not_subset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMPLEX_CHECKS_NOT_SUBSET)
        self.assertRegex(str(complex_checks_not_subset_error), "complexChecksNotSubset")

        complex_checks_not_subset_two_error = error_collection.get_error_by_command_name(
            "complexChecksNotSubsetTwo")
        self.assertTrue(complex_checks_not_subset_two_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_ADDITIONAL_COMPLEX_ACCESS_CHECK)
        self.assertRegex(str(complex_checks_not_subset_two_error), "complexChecksNotSubsetTwo")

        complex_resource_pattern_change_error = error_collection.get_error_by_command_name(
            "complexResourcePatternChange")
        self.assertTrue(complex_resource_pattern_change_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMPLEX_PRIVILEGES_NOT_SUBSET)
        self.assertRegex(str(complex_resource_pattern_change_error), "complexResourcePatternChange")

        complex_action_types_not_subset_error = error_collection.get_error_by_command_name(
            "complexActionTypesNotSubset")
        self.assertTrue(complex_action_types_not_subset_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMPLEX_PRIVILEGES_NOT_SUBSET)
        self.assertRegex(str(complex_action_types_not_subset_error), "complexActionTypesNotSubset")

        complex_action_types_not_subset_two_error = error_collection.get_error_by_command_name(
            "complexActionTypesNotSubsetTwo")
        self.assertTrue(complex_action_types_not_subset_two_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMPLEX_PRIVILEGES_NOT_SUBSET)
        self.assertRegex(
            str(complex_action_types_not_subset_two_error), "complexActionTypesNotSubsetTwo")

        additional_complex_access_check_error = error_collection.get_error_by_command_name(
            "additionalComplexAccessCheck")
        self.assertTrue(additional_complex_access_check_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_ADDITIONAL_COMPLEX_ACCESS_CHECK)
        self.assertRegex(str(additional_complex_access_check_error), "additionalComplexAccessCheck")

        removed_access_check_field_error = error_collection.get_error_by_command_name(
            "removedAccessCheckField")
        self.assertTrue(removed_access_check_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_REMOVED_ACCESS_CHECK_FIELD)
        self.assertRegex(str(removed_access_check_field_error), "removedAccessCheckField")

        added_access_check_field_error = error_collection.get_error_by_command_name(
            "addedAccessCheckField")
        self.assertTrue(added_access_check_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_ADDED_ACCESS_CHECK_FIELD)
        self.assertRegex(str(added_access_check_field_error), "addedAccessCheckField")

        missing_array_command_type_old_error = error_collection.get_error_by_command_name(
            "arrayCommandTypeErrorNoArrayOld")
        self.assertTrue(missing_array_command_type_old_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_TYPE_NOT_ARRAY)
        self.assertRegex(str(missing_array_command_type_old_error), "array<ArrayTypeStruct>")

        missing_array_command_type_new_error = error_collection.get_error_by_command_name(
            "arrayCommandTypeErrorNoArrayNew")
        self.assertTrue(missing_array_command_type_new_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_TYPE_NOT_ARRAY)
        self.assertRegex(str(missing_array_command_type_new_error), "array<ArrayTypeStruct>")

        missing_array_command_parameter_old_error = error_collection.get_error_by_command_name(
            "arrayCommandParameterNoArrayOld")
        self.assertTrue(missing_array_command_parameter_old_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_TYPE_NOT_ARRAY)
        self.assertRegex(str(missing_array_command_parameter_old_error), "array<ArrayTypeStruct>")

        missing_array_command_parameter_new_error = error_collection.get_error_by_command_name(
            "arrayCommandParameterNoArrayNew")
        self.assertTrue(missing_array_command_parameter_new_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_TYPE_NOT_ARRAY)
        self.assertRegex(str(missing_array_command_parameter_new_error), "array<ArrayTypeStruct>")

        new_reply_field_missing_unstable_field_error = error_collection.get_error_by_command_name(
            "newReplyFieldMissingUnstableField")
        self.assertTrue(new_reply_field_missing_unstable_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_REQUIRES_STABILITY)
        self.assertRegex(
            str(new_reply_field_missing_unstable_field_error), "newReplyFieldMissingUnstableField")

        new_command_type_field_missing_unstable_field_error = error_collection.get_error_by_command_name(
            "newCommandTypeFieldMissingUnstableField")
        self.assertTrue(new_command_type_field_missing_unstable_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRES_STABILITY)
        self.assertRegex(
            str(new_command_type_field_missing_unstable_field_error),
            "newCommandTypeFieldMissingUnstableField")

        new_parameter_missing_unstable_field_error = error_collection.get_error_by_command_name(
            "newParameterMissingUnstableField")
        self.assertTrue(new_parameter_missing_unstable_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_PARAMETER_REQUIRES_STABILITY)
        self.assertRegex(
            str(new_parameter_missing_unstable_field_error), "newParameterMissingUnstableField")

        added_new_reply_field_missing_unstable_field_error = error_collection.get_error_by_command_name(
            "addedNewReplyFieldMissingUnstableField")
        self.assertTrue(added_new_reply_field_missing_unstable_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_REQUIRES_STABILITY)
        self.assertRegex(
            str(added_new_reply_field_missing_unstable_field_error),
            "addedNewReplyFieldMissingUnstableField")

        added_new_command_type_field_missing_unstable_field_error = error_collection.get_error_by_command_name(
            "addedNewCommandTypeFieldMissingUnstableField")
        self.assertTrue(added_new_command_type_field_missing_unstable_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRES_STABILITY)
        self.assertRegex(
            str(added_new_command_type_field_missing_unstable_field_error),
            "addedNewCommandTypeFieldMissingUnstableField")

        added_new_parameter_missing_unstable_field_error = error_collection.get_error_by_command_name(
            "addedNewParameterMissingUnstableField")
        self.assertTrue(added_new_parameter_missing_unstable_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_PARAMETER_REQUIRES_STABILITY)
        self.assertRegex(
            str(added_new_parameter_missing_unstable_field_error),
            "addedNewParameterMissingUnstableField")

        chained_struct_incompatible_error = error_collection.get_error_by_command_name(
            "chainedStructIncompatible")
        self.assertTrue(chained_struct_incompatible_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_TYPE_NOT_SUPERSET)
        self.assertRegex(str(chained_struct_incompatible_error), "chainedStructIncompatible")

        reply_with_incompatible_chained_struct_error = error_collection.get_error_by_command_name(
            "replyWithIncompatibleChainedStruct")
        self.assertTrue(reply_with_incompatible_chained_struct_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_VARIANT_TYPE_NOT_SUBSET)
        self.assertRegex(
            str(reply_with_incompatible_chained_struct_error), "replyWithIncompatibleChainedStruct")

        type_with_incompatible_chained_struct_error = error_collection.get_error_by_command_name(
            "typeWithIncompatibleChainedStruct")
        self.assertTrue(
            type_with_incompatible_chained_struct_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_BSON_SERIALIZATION_TYPE_ANY)
        self.assertRegex(
            str(type_with_incompatible_chained_struct_error), "typeWithIncompatibleChainedStruct")

        incompatible_chained_type_error = error_collection.get_error_by_command_name(
            "incompatibleChainedType")
        self.assertTrue(incompatible_chained_type_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_TYPE_NOT_SUPERSET)
        self.assertRegex(str(incompatible_chained_type_error), "incompatibleChainedType")

        new_parameter_removed_chained_type_error = error_collection.get_error_by_command_name(
            "newParameterRemovedChainedType")
        self.assertTrue(
            new_parameter_removed_chained_type_error.error_id ==
            idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAMETER_CHAINED_TYPE_NOT_SUPERSET)
        self.assertRegex(
            str(new_parameter_removed_chained_type_error), "newParameterRemovedChainedType")

        new_reply_added_chained_type_error = error_collection.get_error_by_command_name(
            "newReplyAddedChainedType")
        self.assertTrue(new_reply_added_chained_type_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_CHAINED_TYPE_NOT_SUBSET)
        self.assertRegex(str(new_reply_added_chained_type_error), "newReplyAddedChainedType")

        optional_bool_to_bool_parameter_error = error_collection.get_error_by_command_name(
            "optionalBoolToBoolParameter")
        self.assertTrue(optional_bool_to_bool_parameter_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_COMMAND_PARAMETER_REQUIRED)

        optional_bool_to_bool_command_type_error = error_collection.get_error_by_command_name(
            "optionalBoolToBoolCommandType")
        self.assertTrue(optional_bool_to_bool_command_type_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_REQUIRED)

        bool_to_optional_bool_reply_error = error_collection.get_error_by_command_name(
            "boolToOptionalBoolReply")
        self.assertTrue(bool_to_optional_bool_reply_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_OPTIONAL)

        unstable_to_stable_reply_field_error = error_collection.get_error_by_command_name(
            "unstableToStableReplyField")
        self.assertTrue(unstable_to_stable_reply_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_UNSTABLE_REPLY_FIELD_CHANGED_TO_STABLE)
        self.assertRegex(str(unstable_to_stable_reply_field_error), "unstableToStableReplyField")

        unstable_to_stable_param_field_error = error_collection.get_error_by_command_name(
            "unstableToStableParamField")
        self.assertTrue(unstable_to_stable_param_field_error.error_id == idl_compatibility_errors.
                        ERROR_ID_UNSTABLE_COMMAND_PARAM_FIELD_CHANGED_TO_STABLE)
        self.assertRegex(str(unstable_to_stable_param_field_error), "unstableToStableParamField")

        unstable_to_stable_type_field_error = error_collection.get_error_by_command_name(
            "unstableToStableTypeField")
        self.assertTrue(unstable_to_stable_type_field_error.error_id == idl_compatibility_errors.
                        ERROR_ID_UNSTABLE_COMMAND_TYPE_FIELD_CHANGED_TO_STABLE)
        self.assertRegex(str(unstable_to_stable_type_field_error), "unstableToStableTypeField")

        new_reply_field_added_as_stable_error = error_collection.get_error_by_command_name(
            "newStableReplyFieldAdded")
        self.assertTrue(new_reply_field_added_as_stable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_ADDED_AS_STABLE)
        self.assertRegex(str(new_reply_field_added_as_stable_error), "newStableReplyFieldAdded")

        new_command_param_field_added_as_stable_error = error_collection.get_error_by_command_name(
            "newStableParameterAdded")
        self.assertTrue(new_command_param_field_added_as_stable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_PARAM_FIELD_ADDED_AS_STABLE)
        self.assertRegex(
            str(new_command_param_field_added_as_stable_error), "newStableParameterAdded")

        new_command_type_field_added_as_stable_error = error_collection.get_error_by_command_name(
            "newStableTypeFieldAdded")
        self.assertTrue(new_command_type_field_added_as_stable_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_NEW_COMMAND_TYPE_FIELD_ADDED_AS_STABLE)
        self.assertRegex(
            str(new_command_type_field_added_as_stable_error), "newStableTypeFieldAdded")

        self.assertEqual(error_collection.count(), 207)

    def test_generic_argument_compatibility_pass(self):
        """Tests that compatible old and new generic_argument.idl files should pass."""
        dir_path = path.dirname(path.realpath(__file__))
        self.assertFalse(
            idl_check_compatibility.check_generic_arguments_compatibility(
                path.join(dir_path,
                          "compatibility_test_pass/old_generic_argument/generic_argument.idl"),
                path.join(dir_path,
                          "compatibility_test_pass/new_generic_argument/generic_argument.idl")).
            has_errors())

    def test_generic_argument_compatibility_fail(self):
        """Tests that incompatible old and new generic_argument.idl files should fail."""
        dir_path = path.dirname(path.realpath(__file__))
        error_collection = idl_check_compatibility.check_generic_arguments_compatibility(
            path.join(dir_path,
                      "compatibility_test_fail/old_generic_argument/generic_argument.idl"),
            path.join(dir_path,
                      "compatibility_test_fail/new_generic_argument/generic_argument.idl"))

        self.assertTrue(error_collection.has_errors())
        self.assertTrue(error_collection.count() == 2)

        removed_generic_argument_error = error_collection.get_error_by_command_name(
            "removedGenericArgument")
        self.assertTrue(removed_generic_argument_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_GENERIC_ARGUMENT_REMOVED)
        self.assertRegex(str(removed_generic_argument_error), "removedGenericArgument")

        removed_generic_reply_field_error = error_collection.get_error_by_command_name(
            "removedGenericReplyField")
        self.assertTrue(removed_generic_reply_field_error.error_id ==
                        idl_compatibility_errors.ERROR_ID_GENERIC_ARGUMENT_REMOVED_REPLY_FIELD)
        self.assertRegex(str(removed_generic_reply_field_error), "removedGenericReplyField")

    def test_error_reply(self):
        """Tests the compatibility checker with the ErrorReply struct."""
        dir_path = path.dirname(path.realpath(__file__))

        self.assertFalse(
            idl_check_compatibility.check_error_reply(
                path.join(dir_path, "compatibility_test_pass/old/error_reply.idl"),
                path.join(dir_path, "compatibility_test_pass/new/error_reply.idl"), [],
                []).has_errors())

        error_collection_fail = idl_check_compatibility.check_error_reply(
            path.join(dir_path, "compatibility_test_fail/old/error_reply.idl"),
            path.join(dir_path, "compatibility_test_fail/new/error_reply.idl"), [], [])

        self.assertTrue(error_collection_fail.has_errors())
        self.assertTrue(error_collection_fail.count() == 1)

        new_error_reply_field_optional_error = error_collection_fail.get_error_by_error_id(
            idl_compatibility_errors.ERROR_ID_NEW_REPLY_FIELD_OPTIONAL)
        self.assertRegex(str(new_error_reply_field_optional_error), "n/a")


if __name__ == '__main__':
    unittest.main()
