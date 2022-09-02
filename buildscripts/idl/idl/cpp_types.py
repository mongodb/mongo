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
"""IDL C++ Code Generator."""

from abc import ABCMeta, abstractmethod

import textwrap
from typing import Any, List, Optional

from . import ast
from . import bson
from . import common
from . import writer

_STD_ARRAY_UINT8_16 = 'std::array<std::uint8_t,16>'


def is_primitive_scalar_type(cpp_type):
    # type: (str) -> bool
    """
    Return True if a cpp_type is a primitive scalar type.

    Primitive scalar types need to have a default value to prevent warnings from Coverity.
    """
    cpp_type = cpp_type.replace(' ', '')
    # TODO (SERVER-50101): Remove 'multiversion::FeatureCompatibilityVersion' once IDL supports
    # a commmand cpp_type of C++ enum.
    return cpp_type in [
        'bool', 'double', 'std::int32_t', 'std::uint32_t', 'std::uint64_t', 'std::int64_t',
        'multiversion::FeatureCompatibilityVersion'
    ]


def is_primitive_type(cpp_type):
    # type: (str) -> bool
    """Return True if a cpp_type is a primitive type and should not be returned as reference."""
    cpp_type = cpp_type.replace(' ', '')
    return is_primitive_scalar_type(cpp_type) or cpp_type == _STD_ARRAY_UINT8_16


def _qualify_optional_type(cpp_type):
    # type: (str) -> str
    """Qualify the type as optional."""
    return 'boost::optional<%s>' % (cpp_type)


def _qualify_array_type(cpp_type):
    # type: (str) -> str
    """Qualify the type if the field is an array."""
    return "std::vector<%s>" % (cpp_type)


def _optionally_make_call(method_name, param):
    # type: (str, str) -> str
    """Return a call to method_name if it is not None, otherwise return an empty string."""
    if not method_name:
        return ''

    return "%s(%s);" % (method_name, param)


class CppTypeBase(metaclass=ABCMeta):
    """Base type for C++ Type information."""

    def __init__(self, field, cpp_type_name):
        # type: (ast.Field, str) -> None
        """Construct a CppTypeBase."""
        self._field = field
        self._cpp_type_name = cpp_type_name

    @abstractmethod
    def get_type_name(self):
        # type: () -> str
        """Get the C++ type name for a field."""
        pass

    @abstractmethod
    def get_storage_type(self):
        # type: () -> str
        """Get the C++ type name for the storage of class member for a field."""
        pass

    @abstractmethod
    def get_getter_setter_type(self):
        # type: () -> str
        """Get the C++ type name for the getter/setter parameter for a field."""
        pass

    @abstractmethod
    def return_by_reference(self):
        # type: () -> bool
        """Return True if the type should be returned by reference."""
        pass

    @abstractmethod
    def is_view_type(self):
        # type: () -> bool
        """Return True if the C++ is returned as a view type from an IDL class."""
        pass

    @abstractmethod
    def get_getter_body(self, member_name):
        # type: (str) -> str
        """Get the body of the getter."""
        pass

    @abstractmethod
    def get_setter_body(self, member_name, validator_method_name):
        # type: (str, str) -> str
        """Get the body of the setter."""
        pass

    @abstractmethod
    def get_transform_to_getter_type(self, expression):
        # type: (str) -> Optional[str]
        """Get the expression to transform the input expression into the getter type."""
        pass

    @abstractmethod
    def get_transform_to_storage_type(self, expression):
        # type: (str) -> Optional[str]
        """Get the expression to transform the input expression into the setter type."""
        pass


class _CppTypeBasic(CppTypeBase):
    """Default class for C++ Type information. Does not handle view types."""

    def get_type_name(self):
        # type: () -> str
        return self._cpp_type_name

    def get_storage_type(self):
        # type: () -> str
        return self.get_type_name()

    def get_getter_setter_type(self):
        # type: () -> str
        return self.get_type_name()

    def return_by_reference(self):
        # type: () -> bool
        return not is_primitive_type(self.get_type_name()) and not self._field.type.is_enum

    def is_view_type(self):
        # type: () -> bool
        return False

    def get_getter_body(self, member_name):
        # type: (str) -> str
        return common.template_args('return ${member_name};', member_name=member_name)

    def get_setter_body(self, member_name, validator_method_name):
        # type: (str, str) -> str
        return common.template_args(
            '${optionally_call_validator} ${member_name} = std::move(value);',
            optionally_call_validator=_optionally_make_call(validator_method_name,
                                                            'value'), member_name=member_name)

    def get_transform_to_getter_type(self, expression):
        # type: (str) -> Optional[str]
        return None

    def get_transform_to_storage_type(self, expression):
        # type: (str) -> Optional[str]
        return None


class _CppTypeView(CppTypeBase):
    """Base type for C++ View Types information."""

    def __init__(self, field, cpp_type_name, storage_type, view_type):
        # type: (ast.Field, str, str, str) -> None
        self._storage_type = storage_type
        self._view_type = view_type
        super(_CppTypeView, self).__init__(field, cpp_type_name)

    def get_type_name(self):
        # type: () -> str
        return self._storage_type

    def get_storage_type(self):
        # type: () -> str
        return self._storage_type

    def get_getter_setter_type(self):
        # type: () -> str
        return self._view_type

    def return_by_reference(self):
        # type: () -> bool
        return False

    def is_view_type(self):
        # type: () -> bool
        return True

    def get_getter_body(self, member_name):
        # type: (str) -> str
        return common.template_args('return ${member_name};', member_name=member_name)

    def get_setter_body(self, member_name, validator_method_name):
        # type: (str, str) -> str
        return common.template_args(
            'auto _tmpValue = ${value}; ${optionally_call_validator} ${member_name} = std::move(_tmpValue);',
            member_name=member_name, optionally_call_validator=_optionally_make_call(
                validator_method_name,
                '_tmpValue'), value=self.get_transform_to_storage_type("value"))

    def get_transform_to_getter_type(self, expression):
        # type: (str) -> Optional[str]
        return None

    def get_transform_to_storage_type(self, expression):
        # type: (str) -> Optional[str]
        return common.template_args(
            '${expression}.toString()',
            expression=expression,
        )


class _CppTypeVector(CppTypeBase):
    """Base type for C++ Std::Vector Types information."""

    def __init__(self, field):
        # type: (ast.Field) -> None
        super(_CppTypeVector, self).__init__(field, 'std::vector<std::uint8_t>')

    def get_type_name(self):
        # type: () -> str
        return self._cpp_type_name

    def get_storage_type(self):
        # type: () -> str
        return self.get_type_name()

    def get_getter_setter_type(self):
        # type: () -> str
        return 'ConstDataRange'

    def return_by_reference(self):
        # type: () -> bool
        return False

    def is_view_type(self):
        # type: () -> bool
        return True

    def get_getter_body(self, member_name):
        # type: (str) -> str
        return common.template_args('return ConstDataRange(${member_name});',
                                    member_name=member_name)

    def get_setter_body(self, member_name, validator_method_name):
        # type: (str, str) -> str
        return common.template_args(
            'auto _tmpValue = ${value}; ${optionally_call_validator} ${member_name} = std::move(_tmpValue);',
            member_name=member_name, optionally_call_validator=_optionally_make_call(
                validator_method_name,
                '_tmpValue'), value=self.get_transform_to_storage_type("value"))

    def get_transform_to_getter_type(self, expression):
        # type: (str) -> Optional[str]
        return common.template_args('ConstDataRange(${expression});', expression=expression)

    def get_transform_to_storage_type(self, expression):
        # type: (str) -> Optional[str]
        return common.template_args(
            'std::vector<std::uint8_t>(reinterpret_cast<const uint8_t*>(${expression}.data()), ' +
            'reinterpret_cast<const uint8_t*>(${expression}.data()) + ${expression}.length())',
            expression=expression)


class _CppTypeDelegating(CppTypeBase):
    """Delegates all method calls a nested instance of CppTypeBase. Used to build other classes."""

    def __init__(self, base, field, cpp_type_name):
        # type: (CppTypeBase, ast.Field, str) -> None
        self._base = base
        super(_CppTypeDelegating, self).__init__(field, cpp_type_name)

    def get_type_name(self):
        # type: () -> str
        return self._base.get_type_name()

    def get_storage_type(self):
        # type: () -> str
        return self._base.get_storage_type()

    def get_getter_setter_type(self):
        # type: () -> str
        return self._base.get_getter_setter_type()

    def return_by_reference(self):
        # type: () -> bool
        return self._base.return_by_reference()

    def is_view_type(self):
        # type: () -> bool
        return self._base.is_view_type()

    def get_getter_body(self, member_name):
        # type: (str) -> str
        return self._base.get_getter_body(member_name)

    def get_setter_body(self, member_name, validator_method_name):
        # type: (str, str) -> str
        return self._base.get_setter_body(member_name, validator_method_name)

    def get_transform_to_getter_type(self, expression):
        # type: (str) -> Optional[str]
        return self._base.get_transform_to_getter_type(expression)

    def get_transform_to_storage_type(self, expression):
        # type: (str) -> Optional[str]
        return self._base.get_transform_to_storage_type(expression)


class _CppTypeArray(_CppTypeDelegating):
    """C++ Array type for wrapping a base C++ Type information."""

    def get_storage_type(self):
        # type: () -> str
        return _qualify_array_type(self._base.get_storage_type())

    def get_getter_setter_type(self):
        # type: () -> str
        return _qualify_array_type(self._base.get_getter_setter_type())

    def return_by_reference(self):
        # type: () -> bool
        if self._base.is_view_type():
            return False
        return True

    def get_getter_body(self, member_name):
        # type: (str) -> str
        convert = self.get_transform_to_getter_type(member_name)
        if convert:
            return common.template_args('return ${convert};', convert=convert)
        return self._base.get_getter_body(member_name)

    def get_setter_body(self, member_name, validator_method_name):
        # type: (str, str) -> str
        convert = self.get_transform_to_storage_type("value")
        if convert:
            return common.template_args(
                'auto _tmpValue = ${convert}; ${optionally_call_validator} ${member_name} = std::move(_tmpValue);',
                member_name=member_name, optionally_call_validator=_optionally_make_call(
                    validator_method_name, '_tmpValue'), convert=convert)
        return self._base.get_setter_body(member_name, validator_method_name)

    def get_transform_to_getter_type(self, expression):
        # type: (str) -> Optional[str]
        if self._base.get_storage_type() != self._base.get_getter_setter_type():
            return common.template_args(
                'transformVector(${expression})',
                expression=expression,
            )
        return None

    def get_transform_to_storage_type(self, expression):
        # type: (str) -> Optional[str]
        if self._base.get_storage_type() != self._base.get_getter_setter_type():
            return common.template_args(
                'transformVector(${expression})',
                expression=expression,
            )
        return None


class _CppTypeOptional(_CppTypeDelegating):
    """Base type for Optional C++ Type information which wraps C++ types."""

    def get_storage_type(self):
        # type: () -> str
        return _qualify_optional_type(self._base.get_storage_type())

    def get_getter_setter_type(self):
        # type: () -> str
        return _qualify_optional_type(self._base.get_getter_setter_type())

    def return_by_reference(self):
        # type: () -> bool
        if self._base.is_view_type():
            return False
        return self._base.return_by_reference()

    def get_getter_body(self, member_name):
        # type: (str) -> str
        base_expression = common.template_args("${member_name}.get()", member_name=member_name)

        convert = self._base.get_transform_to_getter_type(base_expression)
        if convert:
            # We need to convert between two different types of optional<T> and yet provide
            # the ability for the user specifiy an uninitialized optional. This occurs
            # for vector<mongo::StringData> and vector<std::string> paired together.
            return common.template_args(
                textwrap.dedent("""\
                if (${member_name}.is_initialized()) {
                    return ${convert};
                } else {
                    return boost::none;
                }
                """), member_name=member_name, convert=convert)
        elif self.is_view_type():
            # For optionals around view types, do an explicit construction
            return common.template_args('return ${param_type}{${member_name}};',
                                        param_type=self.get_getter_setter_type(),
                                        member_name=member_name)
        return common.template_args('return ${member_name};', member_name=member_name)

    def get_setter_body(self, member_name, validator_method_name):
        # type: (str, str) -> str
        convert = self._base.get_transform_to_storage_type("value.get()")
        if convert or validator_method_name:
            if not convert:
                convert = "value.get()"
            return common.template_args(
                textwrap.dedent("""\
                            if (value.is_initialized()) {
                                auto _tmpValue = ${convert};
                                ${optionally_call_validator}
                                ${member_name} = std::move(_tmpValue);
                            } else {
                                ${member_name} = boost::none;
                            }
                            """), member_name=member_name, convert=convert,
                optionally_call_validator=_optionally_make_call(validator_method_name, '_tmpValue'))
        return self._base.get_setter_body(member_name, validator_method_name)


def get_cpp_type_from_cpp_type_name(field, cpp_type_name, array):
    # type: (ast.Field, str, bool) -> CppTypeBase
    """Get the C++ Type information for the given C++ type name, e.g. std::string."""
    cpp_type_info: CppTypeBase
    if cpp_type_name == 'std::string':
        cpp_type_info = _CppTypeView(field, 'std::string', 'std::string', 'StringData')
    elif cpp_type_name == 'std::vector<std::uint8_t>':
        cpp_type_info = _CppTypeVector(field)
    else:
        cpp_type_info = _CppTypeBasic(field, cpp_type_name)

    if array:
        cpp_type_info = _CppTypeArray(cpp_type_info, field, cpp_type_name)

    return cpp_type_info


def get_cpp_type_without_optional(field):
    # type: (ast.Field) -> CppTypeBase
    """Get the C++ Type information for the given field but ignore optional."""
    return get_cpp_type_from_cpp_type_name(field, field.type.cpp_type, field.type.is_array)


def get_cpp_type(field):
    # type: (ast.Field) -> CppTypeBase
    """Get the C++ Type information for the given field."""

    cpp_type_info = get_cpp_type_without_optional(field)

    if field.optional:
        return _CppTypeOptional(cpp_type_info, field, field.type.cpp_type)

    return cpp_type_info


class BsonCppTypeBase(object, metaclass=ABCMeta):
    """Base type for custom C++ support for BSON Types information."""

    def __init__(self, ast_type):
        # type: (ast.Type) -> None
        """Construct a BsonCppTypeBase."""
        self._ast_type = ast_type

    @abstractmethod
    def gen_deserializer_expression(self, indented_writer, object_instance):
        # type: (writer.IndentedTextWriter, str) -> str
        """Generate code with the text writer and return an expression to deserialize the type."""
        pass

    @abstractmethod
    def has_serializer(self):
        # type: () -> bool
        """Return True if this class generate a serializer for the given field."""
        pass

    @abstractmethod
    def gen_serializer_expression(self, indented_writer, expression):
        # type: (writer.IndentedTextWriter, str) -> str
        """Generate code with the text writer and return an expression to serialize the type."""
        pass


def _call_method_or_global_function(expression, method_name):
    # type: (str, str) -> str
    """
    Given a fully-qualified method name, call it correctly.

    A function is prefixed with "::" and use it to indicate a function instead of a method. It is
    not treated as a global C++ function though. This notion of functions is designed to support
    enum deserializers/serializers which are not methods.
    """
    short_method_name = writer.get_method_name(method_name)
    if writer.is_function(method_name):
        return common.template_args('${method_name}(${expression})', expression=expression,
                                    method_name=method_name)

    return common.template_args('${expression}.${method_name}()', expression=expression,
                                method_name=short_method_name)


class _CommonBsonCppTypeBase(BsonCppTypeBase):
    """Custom C++ support for basic BSON types."""

    def __init__(self, ast_type, deserialize_method_name):
        # type: (ast.Type, str) -> None
        self._deserialize_method_name = deserialize_method_name
        super(_CommonBsonCppTypeBase, self).__init__(ast_type)

    def gen_deserializer_expression(self, indented_writer, object_instance):
        # type: (writer.IndentedTextWriter, str) -> str
        return common.template_args('${object_instance}.${method_name}()',
                                    object_instance=object_instance,
                                    method_name=self._deserialize_method_name)

    def has_serializer(self):
        # type: () -> bool
        return self._ast_type.serializer is not None

    def gen_serializer_expression(self, indented_writer, expression):
        # type: (writer.IndentedTextWriter, str) -> str
        return _call_method_or_global_function(expression, self._ast_type.serializer)


class _ObjectBsonCppTypeBase(BsonCppTypeBase):
    """Custom C++ support for object BSON types."""

    def gen_deserializer_expression(self, indented_writer, object_instance):
        # type: (writer.IndentedTextWriter, str) -> str
        if self._ast_type.deserializer:
            # Call a method like: Class::method(const BSONObj& value)
            indented_writer.write_line(
                common.template_args('const BSONObj localObject = ${object_instance}.Obj();',
                                     object_instance=object_instance))
            return "localObject"

        # Just pass the BSONObj through without trying to parse it.
        return common.template_args('${object_instance}.Obj()', object_instance=object_instance)

    def has_serializer(self):
        # type: () -> bool
        return self._ast_type.serializer is not None

    def gen_serializer_expression(self, indented_writer, expression):
        # type: (writer.IndentedTextWriter, str) -> str
        method_name = writer.get_method_name(self._ast_type.serializer)
        indented_writer.write_line(
            common.template_args('const BSONObj localObject = ${expression}.${method_name}();',
                                 expression=expression, method_name=method_name))
        return "localObject"


class _ArrayBsonCppTypeBase(BsonCppTypeBase):
    """Custom C++ support for array BSON types."""

    def gen_deserializer_expression(self, indented_writer, object_instance):
        # type: (writer.IndentedTextWriter, str) -> str
        if self._ast_type.deserializer:
            indented_writer.write_line(
                common.template_args('BSONArray localArray(${object_instance}.Obj());',
                                     object_instance=object_instance))
            return "localArray"

        # Just pass the BSONObj through without trying to parse it.
        return common.template_args('BSONArray(${object_instance}.Obj())',
                                    object_instance=object_instance)

    def has_serializer(self):
        # type: () -> bool
        return self._ast_type.serializer is not None

    def gen_serializer_expression(self, indented_writer, expression):
        # type: (writer.IndentedTextWriter, str) -> str
        method_name = writer.get_method_name(self._ast_type.serializer)
        indented_writer.write_line(
            common.template_args('BSONArray localArray(${expression}.${method_name}());',
                                 expression=expression, method_name=method_name))
        return "localArray"


class _BinDataBsonCppTypeBase(BsonCppTypeBase):
    """Custom C++ support for all binData BSON types."""

    def gen_deserializer_expression(self, indented_writer, object_instance):
        # type: (writer.IndentedTextWriter, str) -> str
        if self._ast_type.bindata_subtype == 'uuid':
            return common.template_args('uassertStatusOK(UUID::parse(${object_instance}))',
                                        object_instance=object_instance)
        return common.template_args('${object_instance}._binDataVector()',
                                    object_instance=object_instance)

    def has_serializer(self):
        # type: () -> bool
        return True

    def gen_serializer_expression(self, indented_writer, expression):
        # type: (writer.IndentedTextWriter, str) -> str
        if self._ast_type.serializer:
            method_name = writer.get_method_name(self._ast_type.serializer)
            indented_writer.write_line(
                common.template_args('ConstDataRange tempCDR = ${expression}.${method_name}();',
                                     expression=expression, method_name=method_name))
        else:
            indented_writer.write_line(
                common.template_args('ConstDataRange tempCDR(${expression});',
                                     expression=expression))

        return common.template_args(
            'BSONBinData(tempCDR.data(), tempCDR.length(), ${bindata_subtype})',
            bindata_subtype=bson.cpp_bindata_subtype_type_name(self._ast_type.bindata_subtype))


# For some types, we want to support custom serialization but defer most of the serialization to
# the core BSONElement class. This means that callers need to only process a string, a vector of
# bytes, or a integer, not a BSONElement or BSONObj.
def get_bson_cpp_type(ast_type):
    # type: (ast.Type) -> Optional[BsonCppTypeBase]
    """Get a class that provides custom serialization for the given BSON type."""

    # Does not support list of types
    if len(ast_type.bson_serialization_type) > 1:
        return None

    if ast_type.bson_serialization_type[0] == 'string':
        return _CommonBsonCppTypeBase(ast_type, "valueStringData")

    if ast_type.bson_serialization_type[0] == 'object':
        return _ObjectBsonCppTypeBase(ast_type)

    if ast_type.bson_serialization_type[0] == 'array':
        return _ArrayBsonCppTypeBase(ast_type)

    if ast_type.bson_serialization_type[0] == 'bindata':
        return _BinDataBsonCppTypeBase(ast_type)

    if ast_type.bson_serialization_type[0] == 'int':
        return _CommonBsonCppTypeBase(ast_type, "_numberInt")

    # Unsupported type
    return None
