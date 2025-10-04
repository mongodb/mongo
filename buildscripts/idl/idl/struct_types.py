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
"""Provide code generation information for structs and commands in a polymorphic way."""

import textwrap
from abc import ABCMeta, abstractmethod

from . import ast, common, cpp_types


def _is_required_constructor_arg(field):
    # type: (ast.Field) -> bool
    """Get whether we require this field to have a value set for constructor purposes."""
    return (
        not field.ignore
        and not field.optional
        and not field.default
        and not field.chained
        and not field.chained_struct_field
        and not field.serialize_op_msg_request_only
    )


def _get_arg_for_field(field):
    # type: (ast.Field) -> str
    """Generate a moveable parameter."""
    cpp_type_info = cpp_types.get_cpp_type(field)
    # Use the storage type for the constructor argument since the generated code will use std::move.
    member_type = cpp_type_info.get_storage_type()

    return "%s %s" % (member_type, common.camel_case(field.cpp_name))


def _get_required_parameters(struct):
    # type: (ast.Struct) -> List[str]
    """Get a list of arguments for required parameters."""
    params = [
        _get_arg_for_field(field) for field in struct.fields if _is_required_constructor_arg(field)
    ]
    # Since this contains defaults, we need to push this to the end of the list.
    params.append(_get_serialization_ctx_arg())
    return params


def _get_serialization_ctx_arg():
    return "boost::optional<SerializationContext> serializationContext = boost::none"


class ArgumentInfo(object):
    """Class that encapsulates information about an argument to a method."""

    def __init__(self, arg):
        # type: (str) -> None
        """Create a instance of the ArgumentInfo class by parsing the argument string."""
        self.defaults = None
        equal_tokens = arg.split("=")
        if len(equal_tokens) > 1:
            self.defaults = equal_tokens[-1].strip()

        space_tokens = equal_tokens[0].strip().split(" ")
        self.type = " ".join(space_tokens[0:-1])
        self.name = space_tokens[-1]

    def get_string(self, get_defaults):
        # type: (bool) -> str
        """Return a formatted argument string."""
        if self.defaults and get_defaults:
            return "%s %s = %s" % (self.type, self.name, self.defaults)  # type: ignore
        return "%s %s" % (self.type, self.name)  # type: ignore


class MethodInfo(object):
    """Class that encapslates information about a method and how to declare, define, and call it."""

    def __init__(
        self,
        class_name,
        method_name,
        args,
        return_type=None,
        static=False,
        const=False,
        explicit=False,
        desc_for_comment=None,
    ):
        # type: (str, str, List[str], str, bool, bool, bool, Optional[str]) -> None
        """Create a MethodInfo instance."""
        self.class_name = class_name
        self.method_name = method_name
        self.args = [ArgumentInfo(arg) for arg in args]
        self.return_type = return_type
        self.static = static
        self.const = const
        self.explicit = explicit
        self.desc_for_comment = desc_for_comment

    def get_declaration(self):
        # type: () -> str
        """Get a declaration for a method."""
        pre_modifiers = ""
        post_modifiers = ""
        return_type_str = ""

        if self.static:
            pre_modifiers = "static "

        if self.const:
            post_modifiers = " const"

        if self.explicit:
            pre_modifiers += "explicit "

        if self.return_type:
            return_type_str = self.return_type + " "

        args = ", ".join([arg.get_string(True) for arg in self.args])
        return f"{pre_modifiers}{return_type_str}{self.method_name}({args}){post_modifiers};"

    def get_definition(self):
        # type: () -> str
        """Get a definition for a method."""
        post_modifiers = ""
        return_type_str = ""

        if self.const:
            post_modifiers = " const"

        if self.return_type:
            return_type_str = self.return_type + " "

        args = ", ".join([arg.get_string(False) for arg in self.args])
        return f"{return_type_str}{self.class_name}::{self.method_name}({args}){post_modifiers}"

    def get_call(self, obj):
        # type: (Optional[str]) -> str
        """Generate a simple call to the method using the defined args list."""

        args = ", ".join([arg.name for arg in self.args])

        if obj:
            return f"{obj}.{self.method_name}({args});"

        return f"{self.method_name}({args});"

    def get_desc_for_comment(self):
        # type: () -> Optional[str]
        """Get the description of this method suitable for commenting it."""
        return self.desc_for_comment


class StructTypeInfoBase(object, metaclass=ABCMeta):
    """Base class for struct and command code generation."""

    @abstractmethod
    def get_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        """Get the constructor method for a struct."""
        pass

    @abstractmethod
    def get_required_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        """Get the constructor method for a struct with parameters for required fields."""
        pass

    @abstractmethod
    def get_serializer_method(self):
        # type: () -> MethodInfo
        """Get the serializer method for a struct."""
        pass

    @abstractmethod
    def get_to_bson_method(self):
        # type: () -> MethodInfo
        """Get the to_bson method for a struct."""
        pass

    @abstractmethod
    def get_deserializer_static_method(self):
        # type: () -> MethodInfo
        """Get the public static deserializer method for a struct."""
        pass

    @abstractmethod
    def get_sharing_deserializer_static_method(self):
        # type: () -> MethodInfo
        """Get the public static deserializer method for a struct that participates in shared ownership of underlying data we are deserializing from."""
        pass

    @abstractmethod
    def get_owned_deserializer_static_method(self):
        # type: () -> MethodInfo
        """Get the public static deserializer method for a struct that takes exclusive ownership of underlying data we are deserializing from."""
        pass

    @abstractmethod
    def get_deserializer_method(self):
        # type: () -> MethodInfo
        """Get the protected deserializer method for a struct."""
        pass

    @abstractmethod
    def get_op_msg_request_serializer_method(self):
        # type: () -> Optional[MethodInfo]
        """Get the OpMsg serializer method for a struct."""
        pass

    @abstractmethod
    def get_op_msg_request_deserializer_static_method(self):
        # type: () -> Optional[MethodInfo]
        """Get the public static OpMsg deserializer method for a struct."""
        pass

    @abstractmethod
    def get_op_msg_request_deserializer_method(self):
        # type: () -> Optional[MethodInfo]
        """Get the protected OpMsg deserializer method for a struct."""
        pass

    @abstractmethod
    def gen_methods(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the additional methods for a class."""
        pass

    @abstractmethod
    def gen_member(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Generate the additional members for a class."""
        pass

    @abstractmethod
    def gen_serializer(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        """Serialize the first field of a Command."""
        pass

    @abstractmethod
    def gen_namespace_check(self, indented_writer, db_name, element):
        # type: (writer.IndentedTextWriter, str, str) -> None
        """Generate the namespace check predicate for a command."""
        pass


class _StructTypeInfo(StructTypeInfoBase):
    """Class for struct code generation."""

    def __init__(self, struct):
        # type: (ast.Struct) -> None
        """Create a _StructTypeInfo instance."""
        self._struct = struct

    def get_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(class_name, class_name, [_get_serialization_ctx_arg()])

    def get_required_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(class_name, class_name, _get_required_parameters(self._struct))

    def get_sharing_deserializer_static_method(self):
        # type: () -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        comment = textwrap.dedent(f"""\
                Factory function that parses a {class_name} from a BSONObj. A {class_name} parsed
                this way participates in ownership of the data underlying the BSONObj.""")
        return MethodInfo(
            class_name,
            "parseSharingOwnership",
            [
                "const BSONObj& bsonObject",
                f'const IDLParserContext& ctxt = IDLParserContext("{self._struct.name}")',
                "DeserializationContext* dctx = nullptr",
            ],
            class_name,
            static=True,
            desc_for_comment=comment,
        )

    def get_owned_deserializer_static_method(self):
        # type: () -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        comment = textwrap.dedent(f"""\
                Factory function that parses a {class_name} from a BSONObj. A {class_name} parsed
                this way takes ownership of the data underlying the BSONObj.""")
        return MethodInfo(
            class_name,
            "parseOwned",
            [
                "BSONObj&& bsonObject",
                f'const IDLParserContext& ctxt = IDLParserContext("{self._struct.name}")',
                "DeserializationContext* dctx = nullptr",
            ],
            class_name,
            static=True,
            desc_for_comment=comment,
        )

    def get_deserializer_static_method(self):
        # type: () -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        comment = textwrap.dedent(f"""\
                Factory function that parses a {class_name} from a BSONObj. A {class_name} parsed
                this way is strictly a view onto that BSONObj; the BSONObj must be kept valid to
                ensure the validity any members of this struct that point-into the BSONObj (i.e.
                unowned
                objects).""")
        return MethodInfo(
            class_name,
            "parse",
            [
                "const BSONObj& bsonObject",
                f'const IDLParserContext& ctxt = IDLParserContext("{self._struct.name}")',
                "DeserializationContext* dctx = nullptr",
            ],
            class_name,
            static=True,
            desc_for_comment=comment,
        )

    def get_deserializer_method(self):
        # type: () -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(
            class_name,
            "parseProtected",
            [
                "const BSONObj& bsonObject",
                f'const IDLParserContext& ctxt = IDLParserContext("{self._struct.name}")',
                "DeserializationContext* dctx = nullptr",
            ],
            "void",
        )

    def get_serializer_method(self):
        # type: () -> MethodInfo
        args = ["BSONObjBuilder* builder"]
        if self._struct.query_shape_component:
            args.append("const SerializationOptions& options = {}")
        return MethodInfo(
            common.title_case(self._struct.cpp_name), "serialize", args, "void", const=True
        )

    def get_to_bson_method(self):
        # type: () -> MethodInfo
        args = []
        if self._struct.query_shape_component:
            args.append("const SerializationOptions& options = {}")
        return MethodInfo(
            common.title_case(self._struct.cpp_name), "toBSON", args, "BSONObj", const=True
        )

    def get_op_msg_request_serializer_method(self):
        # type: () -> Optional[MethodInfo]
        return None

    def get_op_msg_request_deserializer_static_method(self):
        # type: () -> Optional[MethodInfo]
        return None

    def get_op_msg_request_deserializer_method(self):
        # type: () -> Optional[MethodInfo]
        return None

    def gen_methods(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        pass

    def gen_member(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        pass

    def gen_serializer(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        pass

    def gen_namespace_check(self, indented_writer, db_name, element):
        # type: (writer.IndentedTextWriter, str, str) -> None
        pass


class _CommandBaseTypeInfo(_StructTypeInfo):
    """Base class for command code generation."""

    def __init__(self, command):
        # type: (ast.Command) -> None
        """Create a _CommandBaseTypeInfo instance."""
        self._command = command

        super(_CommandBaseTypeInfo, self).__init__(command)

    def get_op_msg_request_serializer_method(self):
        # type: () -> Optional[MethodInfo]
        return MethodInfo(
            common.title_case(self._struct.cpp_name), "serialize", [], "OpMsgRequest", const=True
        )

    def get_op_msg_request_deserializer_static_method(self):
        # type: () -> Optional[MethodInfo]
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(
            class_name,
            "parse",
            [
                "const OpMsgRequest& request",
                f'const IDLParserContext& ctxt = IDLParserContext("{self._struct.name}")',
                "DeserializationContext* dctx = nullptr",
            ],
            class_name,
            static=True,
        )

    def get_op_msg_request_deserializer_method(self):
        # type: () -> Optional[MethodInfo]
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(
            class_name,
            "parseProtected",
            [
                "const OpMsgRequest& request",
                f'const IDLParserContext& ctxt = IDLParserContext("{self._struct.name}")',
                "DeserializationContext* dctx = nullptr",
            ],
            "void",
        )


class _IgnoredCommandTypeInfo(_CommandBaseTypeInfo):
    """Class for command code generation."""

    def __init__(self, command):
        # type: (ast.Command) -> None
        """Create a _IgnoredCommandTypeInfo instance."""
        self._command = command

        super(_IgnoredCommandTypeInfo, self).__init__(command)

    def get_serializer_method(self):
        # type: () -> MethodInfo
        return MethodInfo(
            common.title_case(self._struct.cpp_name),
            "serialize",
            ["BSONObjBuilder* builder"],
            "void",
            const=True,
        )

    def get_to_bson_method(self):
        # type: () -> MethodInfo
        # Commands that require namespaces require it as a parameter to serialize()
        return MethodInfo(
            common.title_case(self._struct.cpp_name), "toBSON", [], "BSONObj", const=True
        )

    def gen_serializer(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line('builder->append("%s"_sd, 1);' % (self._command.name))

    def gen_namespace_check(self, indented_writer, db_name, element):
        # type: (writer.IndentedTextWriter, str, str) -> None
        pass


def _get_command_type_parameter(command, gen_header=False):
    # type: (ast.Command, bool) -> str
    """Get the parameter for the command type."""
    cpp_type_info = cpp_types.get_cpp_type(command.command_field)
    # Use the storage type for the constructor argument since the generated code will use std::move.
    member_type = cpp_type_info.get_storage_type()
    result = f"{member_type} {common.camel_case(command.command_field.cpp_name)}"
    if not gen_header or "&" in result:
        result = "const " + result
    return result


class _CommandFromType(_CommandBaseTypeInfo):
    """Class for command code generation for custom type."""

    def __init__(self, command):
        # type: (ast.Command) -> None
        """Create a _CommandFromType instance."""
        assert command.command_field
        self._command = command
        super(_CommandFromType, self).__init__(command)

    def get_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)

        arg = _get_command_type_parameter(self._command, gen_header)
        sc_arg = _get_serialization_ctx_arg()
        return MethodInfo(class_name, class_name, [arg, sc_arg], explicit=True)

    def get_required_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)

        arg = _get_command_type_parameter(self._command, gen_header)
        return MethodInfo(
            class_name, class_name, [arg] + _get_required_parameters(self._struct), explicit=True
        )

    def get_serializer_method(self):
        # type: () -> MethodInfo
        return MethodInfo(
            common.title_case(self._struct.cpp_name),
            "serialize",
            ["BSONObjBuilder* builder"],
            "void",
            const=True,
        )

    def get_to_bson_method(self):
        # type: () -> MethodInfo
        return MethodInfo(
            common.title_case(self._struct.cpp_name), "toBSON", [], "BSONObj", const=True
        )

    def get_deserializer_method(self):
        # type: () -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(
            class_name,
            "parseProtected",
            [
                "const BSONObj& bsonObject",
                f'const IDLParserContext& ctxt = IDLParserContext("{self._struct.name}")',
                "DeserializationContext* dctx = nullptr",
            ],
            "void",
        )

    def gen_methods(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        raise NotImplementedError

    def gen_member(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        raise NotImplementedError

    def gen_serializer(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        raise NotImplementedError

    def gen_namespace_check(self, indented_writer, db_name, element):
        # type: (writer.IndentedTextWriter, str, str) -> None
        # TODO: should the name of the first element be validated??
        raise NotImplementedError


class _CommandWithNamespaceTypeInfo(_CommandBaseTypeInfo):
    """Class for command code generation."""

    def __init__(self, command):
        # type: (ast.Command) -> None
        """Create a _CommandWithNamespaceTypeInfo instance."""
        self._command = command

        super(_CommandWithNamespaceTypeInfo, self).__init__(command)

    @staticmethod
    def _get_nss_param(gen_header):
        nss_param = "NamespaceString nss"
        if not gen_header:
            nss_param = "const " + nss_param
        return nss_param

    def get_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        sc_arg = _get_serialization_ctx_arg()
        return MethodInfo(
            class_name, class_name, [self._get_nss_param(gen_header), sc_arg], explicit=True
        )

    def get_required_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(
            class_name,
            class_name,
            [self._get_nss_param(gen_header)] + _get_required_parameters(self._struct),
        )

    def get_serializer_method(self):
        # type: () -> MethodInfo
        return MethodInfo(
            common.title_case(self._struct.cpp_name),
            "serialize",
            ["BSONObjBuilder* builder"],
            "void",
            const=True,
        )

    def get_to_bson_method(self):
        # type: () -> MethodInfo
        return MethodInfo(
            common.title_case(self._struct.cpp_name), "toBSON", [], "BSONObj", const=True
        )

    def get_deserializer_method(self):
        # type: () -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(
            class_name,
            "parseProtected",
            [
                "const BSONObj& bsonObject",
                f'const IDLParserContext& ctxt = IDLParserContext("{self._struct.name}")',
                "DeserializationContext* dctx = nullptr",
            ],
            "void",
        )

    def gen_methods(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line(
            "void setNamespace(NamespaceString nss) { _nss = std::move(nss); }"
        )
        indented_writer.write_line("const NamespaceString& getNamespace() const { return _nss; }")
        if self._struct.non_const_getter:
            indented_writer.write_line("NamespaceString& getNamespace() { return _nss; }")

    def gen_member(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line("NamespaceString _nss;")

    def gen_serializer(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        if self._struct.allow_global_collection_name:
            indented_writer.write_line(
                '_nss.serializeCollectionName(builder, "%s"_sd);' % (self._command.name)
            )
        else:
            indented_writer.write_line("invariant(!_nss.isEmpty());")
            indented_writer.write_line(
                'builder->append("%s"_sd, _nss.coll());' % (self._command.name)
            )
        indented_writer.write_empty_line()

    def gen_namespace_check(self, indented_writer, db_name, element):
        # type: (writer.IndentedTextWriter, str, str) -> None
        # TODO: should the name of the first element be validated??
        indented_writer.write_line("invariant(_nss.isEmpty());")
        allow_global = "true" if self._struct.allow_global_collection_name else "false"
        indented_writer.write_line(
            "auto collectionName = ctxt.checkAndAssertCollectionName(%s, %s);"
            % (element, allow_global)
        )
        indented_writer.write_line(
            "_nss = NamespaceStringUtil::deserialize(%s, collectionName);" % (db_name)
        )
        indented_writer.write_line(
            'uassert(ErrorCodes::InvalidNamespace, str::stream() << "Invalid namespace specified: "'
            " << _nss.toStringForErrorMsg(), _nss.isValid());"
        )


class _CommandWithUUIDNamespaceTypeInfo(_CommandBaseTypeInfo):
    """Class for command with namespace or UUID code generation."""

    def __init__(self, command):
        # type: (ast.Command) -> None
        """Create a _CommandWithUUIDNamespaceTypeInfo instance."""
        self._command = command

        super(_CommandWithUUIDNamespaceTypeInfo, self).__init__(command)

    @staticmethod
    def _get_nss_param(gen_header):
        nss_param = "NamespaceStringOrUUID nssOrUUID"
        if not gen_header:
            nss_param = "const " + nss_param
        return nss_param

    def get_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        sc_arg = _get_serialization_ctx_arg()
        return MethodInfo(
            class_name, class_name, [self._get_nss_param(gen_header), sc_arg], explicit=True
        )

    def get_required_constructor_method(self, gen_header=False):
        # type: (bool) -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(
            class_name,
            class_name,
            [self._get_nss_param(gen_header)] + _get_required_parameters(self._struct),
        )

    def get_serializer_method(self):
        # type: () -> MethodInfo
        return MethodInfo(
            common.title_case(self._struct.cpp_name),
            "serialize",
            ["BSONObjBuilder* builder"],
            "void",
            const=True,
        )

    def get_to_bson_method(self):
        # type: () -> MethodInfo
        return MethodInfo(
            common.title_case(self._struct.cpp_name), "toBSON", [], "BSONObj", const=True
        )

    def get_deserializer_method(self):
        # type: () -> MethodInfo
        class_name = common.title_case(self._struct.cpp_name)
        return MethodInfo(
            class_name,
            "parseProtected",
            [
                "const BSONObj& bsonObject",
                f'const IDLParserContext& ctxt = IDLParserContext("{self._struct.name}")',
                "DeserializationContext* dctx = nullptr",
            ],
            "void",
        )

    def gen_methods(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line(
            "const NamespaceStringOrUUID& getNamespaceOrUUID() const { return _nssOrUUID; }"
        )
        if self._struct.non_const_getter:
            indented_writer.write_line(
                "NamespaceStringOrUUID& getNamespaceOrUUID() { return _nssOrUUID; }"
            )

    def gen_member(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line("NamespaceStringOrUUID _nssOrUUID;")

    def gen_serializer(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line('_nssOrUUID.serialize(builder, "%s"_sd);' % (self._command.name))
        indented_writer.write_empty_line()

    def gen_namespace_check(self, indented_writer, db_name, element):
        # type: (writer.IndentedTextWriter, str, str) -> None
        indented_writer._stream.write(f"""
    auto collOrUUID = ctxt.checkAndAssertCollectionNameOrUUID({element});
    _nssOrUUID = std::holds_alternative<StringData>(collOrUUID) ? NamespaceStringUtil::deserialize({db_name}, get<StringData>(collOrUUID)) : NamespaceStringOrUUID({db_name}, get<UUID>(collOrUUID));
    uassert(ErrorCodes::InvalidNamespace, str::stream() << "Invalid namespace specified: " << _nssOrUUID.toStringForErrorMsg(), !_nssOrUUID.isNamespaceString() || _nssOrUUID.nss().isValid());
""")


def get_struct_info(struct):
    # type: (ast.Struct) -> StructTypeInfoBase
    """Get type information about the struct or command to generate C++ code."""

    if isinstance(struct, ast.Command):
        if struct.namespace == common.COMMAND_NAMESPACE_IGNORED:
            return _IgnoredCommandTypeInfo(struct)
        elif struct.namespace == common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB:
            return _CommandWithNamespaceTypeInfo(struct)
        elif struct.namespace == common.COMMAND_NAMESPACE_CONCATENATE_WITH_DB_OR_UUID:
            return _CommandWithUUIDNamespaceTypeInfo(struct)
        return _CommandFromType(struct)

    return _StructTypeInfo(struct)
