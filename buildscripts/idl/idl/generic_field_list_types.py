# Copyright (C) 2020-present MongoDB, Inc.
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
"""Provide code generation information for generic arguments and reply fields."""

from . import ast, common
from .struct_types import MethodInfo


class FieldListInfo:
    """Class encapsulating code gen information needed for fields in a special command generic argument/reply struct."""

    def __init__(self, struct):
        # type: (ast.Struct) -> None
        """Create a FieldListInfo instance."""
        self.struct = struct

    def get_has_field_method(self):
        # type: () -> MethodInfo
        """Get the hasField method for a generic argument or generic reply field list."""
        class_name = common.title_case(self.struct.cpp_name)
        return MethodInfo(class_name, "hasField", ["StringData fieldName"], "bool", static=True)

    def get_should_forward_name(self):
        """Get the name of the shard-forwarding rule for a generic argument or reply field."""
        if self.struct.generic_list_type == ast.GenericListType.ARG:
            return "shouldForwardToShards"
        else:
            return "shouldForwardFromShards"

    def lookup_should_forward(self, field):
        if self.struct.generic_list_type == ast.GenericListType.ARG:
            return field.forward_to_shards
        else:
            return field.forward_from_shards

    def get_should_forward_method(self):
        # type: () -> MethodInfo
        """Get the method for checking the shard-forwarding rule of an argument or reply field."""
        class_name = common.title_case(self.struct.cpp_name)
        return MethodInfo(
            class_name,
            self.get_should_forward_name(),
            ["StringData fieldName"],
            "bool",
            static=True,
        )


def get_field_list_info(struct):
    # type: (ast.Struct) -> FieldListInfo
    """Get type information about the generic argument or reply field list to generate C++ code."""

    return FieldListInfo(struct)
