# Copyright (C) 2017 MongoDB Inc.
#
# This program is free software: you can redistribute it and/or  modify
# it under the terms of the GNU Affero General Public License, version 3,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
"""Provide code generation information for structs and commands in a polymorphic way."""

from __future__ import absolute_import, print_function, unicode_literals

from abc import ABCMeta, abstractmethod
from typing import Optional, List

from . import ast
from . import common
from . import writer


class MethodInfo(object):
    """Class that encapslates information about a method and how to declare, define, and call it."""

    def __init__(self, class_name, method_name, args, return_type=None, static=False, const=False):
        # type: (unicode, unicode, List[unicode], unicode, bool, bool) -> None
        # pylint: disable=too-many-arguments
        """Create a MethodInfo instance."""
        self._class_name = class_name
        self._method_name = method_name
        self._args = args
        self._return_type = return_type
        self._static = static
        self._const = const

    def get_declaration(self):
        # type: () -> unicode
        """Get a declaration for a method."""
        pre_modifiers = ''
        post_modifiers = ''
        return_type_str = ''

        if self._static:
            pre_modifiers = 'static '

        if self._const:
            post_modifiers = ' const'

        if self._return_type:
            return_type_str = self._return_type + ' '

        return common.template_args(
            "${pre_modifiers}${return_type}${method_name}(${args})${post_modifiers};",
            pre_modifiers=pre_modifiers,
            return_type=return_type_str,
            method_name=self._method_name,
            args=', '.join(self._args),
            post_modifiers=post_modifiers)

    def get_definition(self):
        # type: () -> unicode
        """Get a definition for a method."""
        pre_modifiers = ''
        post_modifiers = ''
        return_type_str = ''

        if self._const:
            post_modifiers = ' const'

        if self._return_type:
            return_type_str = self._return_type + ' '

        return common.template_args(
            "${pre_modifiers}${return_type}${class_name}::${method_name}(${args})${post_modifiers}",
            pre_modifiers=pre_modifiers,
            return_type=return_type_str,
            class_name=self._class_name,
            method_name=self._method_name,
            args=', '.join(self._args),
            post_modifiers=post_modifiers)

    def get_call(self, obj):
        # type: (Optional[unicode]) -> unicode
        """Generate a simply call to the method using the defined args list."""

        args = ', '.join([a.split(' ')[-1] for a in self._args])

        if obj:
            return common.template_args(
                "${obj}.${method_name}(${args});",
                obj=obj,
                method_name=self._method_name,
                args=args)

        return common.template_args(
            "${method_name}(${args});", method_name=self._method_name, args=args)


class StructTypeInfoBase(object):
    """Base class for struct and command code generation."""

    __metaclass__ = ABCMeta

    @abstractmethod
    def get_constructor_method(self):
        # type: () -> MethodInfo
        """Get the constructor method for a struct."""
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
    def get_deserializer_method(self):
        # type: () -> MethodInfo
        """Get the protected deserializer method for a struct."""
        pass

    @abstractmethod
    def gen_getter_method(self, indented_writer):
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


class _StructTypeInfo(StructTypeInfoBase):
    """Class for struct code generation."""

    def __init__(self, struct):
        # type: (ast.Struct) -> None
        """Create a _StructTypeInfo instance."""
        self._struct = struct

    def get_constructor_method(self):
        # type: () -> MethodInfo
        pass

    def get_serializer_method(self):
        # type: () -> MethodInfo
        return MethodInfo(
            common.title_case(self._struct.name),
            'serialize', ['BSONObjBuilder* builder'],
            'void',
            const=True)

    def get_to_bson_method(self):
        # type: () -> MethodInfo
        return MethodInfo(common.title_case(self._struct.name), 'toBSON', [], 'BSONObj', const=True)

    def get_deserializer_static_method(self):
        # type: () -> MethodInfo
        class_name = common.title_case(self._struct.name)
        return MethodInfo(
            class_name,
            'parse', ['const IDLParserErrorContext& ctxt', 'const BSONObj& bsonObject'],
            class_name,
            static=True)

    def get_deserializer_method(self):
        # type: () -> MethodInfo
        return MethodInfo(
            common.title_case(self._struct.name), 'parseProtected',
            ['const IDLParserErrorContext& ctxt', 'const BSONObj& bsonObject'], 'void')

    def gen_getter_method(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        pass

    def gen_member(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        pass

    def gen_serializer(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        pass


class _IgnoredCommandTypeInfo(_StructTypeInfo):
    """Class for command code generation."""

    def __init__(self, command):
        # type: (ast.Command) -> None
        """Create a _IgnoredCommandTypeInfo instance."""
        self._command = command

        super(_IgnoredCommandTypeInfo, self).__init__(command)

    def get_serializer_method(self):
        # type: () -> MethodInfo
        return super(_IgnoredCommandTypeInfo, self).get_serializer_method()

    def get_to_bson_method(self):
        # type: () -> MethodInfo
        return super(_IgnoredCommandTypeInfo, self).get_to_bson_method()

    def get_deserializer_static_method(self):
        # type: () -> MethodInfo
        return super(_IgnoredCommandTypeInfo, self).get_deserializer_static_method()

    def get_deserializer_method(self):
        # type: () -> MethodInfo
        return super(_IgnoredCommandTypeInfo, self).get_deserializer_method()

    def gen_getter_method(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        pass

    def gen_member(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        pass

    def gen_serializer(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line('builder->append("%s", 1);' % (self._command.name))


class _CommandWithNamespaceTypeInfo(_StructTypeInfo):
    """Class for command code generation."""

    def __init__(self, command):
        # type: (ast.Command) -> None
        """Create a _CommandWithNamespaceTypeInfo instance."""
        self._command = command

        super(_CommandWithNamespaceTypeInfo, self).__init__(command)

    def get_constructor_method(self):
        # type: () -> MethodInfo
        class_name = common.title_case(self._struct.name)
        return MethodInfo(class_name, class_name, ['const NamespaceString& nss'])

    def get_serializer_method(self):
        # type: () -> MethodInfo
        # Commands that require namespaces require it as a parameter to serialize()
        return MethodInfo(
            common.title_case(self._struct.name),
            'serialize', ['BSONObjBuilder* builder'],
            'void',
            const=True)

    def get_to_bson_method(self):
        # type: () -> MethodInfo
        # Commands that require namespaces require it as a parameter to serialize()
        return MethodInfo(common.title_case(self._struct.name), 'toBSON', [], 'BSONObj', const=True)

    def get_deserializer_static_method(self):
        # type: () -> MethodInfo
        # Commands that have concatentate_with_db namespaces require db name as a parameter
        class_name = common.title_case(self._struct.name)
        return MethodInfo(
            class_name,
            'parse',
            ['const IDLParserErrorContext& ctxt', 'StringData dbName', 'const BSONObj& bsonObject'],
            class_name,
            static=True)

    def get_deserializer_method(self):
        # type: () -> MethodInfo
        # Commands that have concatentate_with_db namespaces require db name as a parameter
        return MethodInfo(
            common.title_case(self._struct.name), 'parseProtected',
            ['const IDLParserErrorContext& ctxt', 'StringData dbName',
             'const BSONObj& bsonObject'], 'void')

    def gen_getter_method(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line('const NamespaceString& getNamespace() const { return _nss; }')

    def gen_member(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line('NamespaceString _nss;')

    def gen_serializer(self, indented_writer):
        # type: (writer.IndentedTextWriter) -> None
        indented_writer.write_line('builder->append("%s", _nss.coll());' % (self._command.name))
        indented_writer.write_empty_line()


def get_struct_info(struct):
    # type: (ast.Struct) -> StructTypeInfoBase
    """Get type information about the struct or command to generate C++ code."""

    if isinstance(struct, ast.Command):
        if struct.namespace == common.COMMAND_NAMESPACE_IGNORED:
            return _IgnoredCommandTypeInfo(struct)
        return _CommandWithNamespaceTypeInfo(struct)

    return _StructTypeInfo(struct)
