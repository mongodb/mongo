# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
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
        return MethodInfo(
            class_name, "hasField", ["std::string_view fieldName"], "bool", static=True
        )

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
            ["std::string_view fieldName"],
            "bool",
            static=True,
        )


def get_field_list_info(struct):
    # type: (ast.Struct) -> FieldListInfo
    """Get type information about the generic argument or reply field list to generate C++ code."""

    return FieldListInfo(struct)
