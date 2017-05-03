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
"""IDL C++ Code Generator."""

from __future__ import absolute_import, print_function, unicode_literals

from abc import ABCMeta, abstractmethod
import string
import textwrap
from typing import Any, Optional

from . import ast
from . import bson
from . import common
from . import writer


def _is_primitive_type(cpp_type):
    # type: (unicode) -> bool
    """Return True if a cpp_type is a primitive type and should not be returned as reference."""
    cpp_type = cpp_type.replace(' ', '')
    return cpp_type in [
        'bool', 'double', 'std::int32_t', 'std::uint32_t', 'std::uint64_t', 'std::int64_t',
        'std::array<std::uint8_t,16>'
    ]


def _qualify_optional_type(cpp_type):
    # type: (unicode) -> unicode
    """Qualify the type as optional."""
    return 'boost::optional<%s>' % (cpp_type)


def _qualify_array_type(cpp_type):
    # type: (unicode) -> unicode
    """Qualify the type if the field is an array."""
    return "std::vector<%s>" % (cpp_type)


class CppTypeBase(object):
    """Base type for C++ Type information."""

    __metaclass__ = ABCMeta

    def __init__(self, field):
        # type: (ast.Field) -> None
        """Construct a CppTypeBase."""
        self._field = field

    @abstractmethod
    def get_type_name(self):
        # type: () -> unicode
        """Get the C++ type name for a field."""
        pass

    @abstractmethod
    def get_storage_type(self):
        # type: () -> unicode
        """Get the C++ type name for the storage of class member for a field."""
        pass

    @abstractmethod
    def get_getter_setter_type(self):
        # type: () -> unicode
        """Get the C++ type name for the getter/setter parameter for a field."""
        pass

    @abstractmethod
    def return_by_reference(self):
        # type: () -> bool
        """Return True if the type should be returned by reference."""
        pass

    @abstractmethod
    def disable_xvalue(self):
        # type: () -> bool
        """Return True if the type should have the xvalue getter disabled."""
        pass

    @abstractmethod
    def is_view_type(self):
        # type: () -> bool
        """Return True if the C++ is returned as a view type from an IDL class."""
        pass

    @abstractmethod
    def get_getter_body(self, member_name):
        # type: (unicode) -> unicode
        """Get the body of the getter."""
        pass

    @abstractmethod
    def get_setter_body(self, member_name):
        # type: (unicode) -> unicode
        """Get the body of the setter."""
        pass

    @abstractmethod
    def get_transform_to_getter_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        """Get the expression to transform the input expression into the getter type."""
        pass

    @abstractmethod
    def get_transform_to_storage_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        """Get the expression to transform the input expression into the setter type."""
        pass


class _CppTypeBasic(CppTypeBase):
    """Default class for C++ Type information. Does not handle view types."""

    def __init__(self, field):
        # type: (ast.Field) -> None
        super(_CppTypeBasic, self).__init__(field)

    def get_type_name(self):
        # type: () -> unicode
        if self._field.struct_type:
            cpp_type = common.title_case(self._field.struct_type)
        else:
            cpp_type = self._field.cpp_type

        return cpp_type

    def get_storage_type(self):
        # type: () -> unicode
        return self.get_type_name()

    def get_getter_setter_type(self):
        # type: () -> unicode
        return self.get_type_name()

    def return_by_reference(self):
        # type: () -> bool
        return not _is_primitive_type(self.get_type_name())

    def disable_xvalue(self):
        # type: () -> bool
        return False

    def is_view_type(self):
        # type: () -> bool
        return False

    def get_getter_body(self, member_name):
        # type: (unicode) -> unicode
        return common.template_args('return $member_name;', member_name=member_name)

    def get_setter_body(self, member_name):
        # type: (unicode) -> unicode
        return common.template_args('${member_name} = std::move(value);', member_name=member_name)

    def get_transform_to_getter_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        return None

    def get_transform_to_storage_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        return None


class _CppTypeView(CppTypeBase):
    """Base type for C++ View Types information."""

    def __init__(self, field, storage_type, view_type):
        # type: (ast.Field, unicode, unicode) -> None
        self._storage_type = storage_type
        self._view_type = view_type
        super(_CppTypeView, self).__init__(field)

    def get_type_name(self):
        # type: () -> unicode
        return self._storage_type

    def get_storage_type(self):
        # type: () -> unicode
        return self._storage_type

    def get_getter_setter_type(self):
        # type: () -> unicode
        return self._view_type

    def return_by_reference(self):
        # type: () -> bool
        return False

    def disable_xvalue(self):
        # type: () -> bool
        return True

    def is_view_type(self):
        # type: () -> bool
        return True

    def get_getter_body(self, member_name):
        # type: (unicode) -> unicode
        return common.template_args('return $member_name;', member_name=member_name)

    def get_setter_body(self, member_name):
        # type: (unicode) -> unicode
        return common.template_args(
            '$member_name = ${value};',
            member_name=member_name,
            value=self.get_transform_to_storage_type("value"))

    def get_transform_to_getter_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        return None

    def get_transform_to_storage_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        return common.template_args(
            '$expression.toString()',
            expression=expression, )


class _CppTypeVector(CppTypeBase):
    """Base type for C++ Std::Vector Types information."""

    def __init__(self, field):
        # type: (ast.Field) -> None
        super(_CppTypeVector, self).__init__(field)

    def get_type_name(self):
        # type: () -> unicode
        return 'std::vector<std::uint8_t>'

    def get_storage_type(self):
        # type: () -> unicode
        return self.get_type_name()

    def get_getter_setter_type(self):
        # type: () -> unicode
        return 'ConstDataRange'

    def return_by_reference(self):
        # type: () -> bool
        return False

    def disable_xvalue(self):
        # type: () -> bool
        return True

    def is_view_type(self):
        # type: () -> bool
        return True

    def get_getter_body(self, member_name):
        # type: (unicode) -> unicode
        return common.template_args(
            'return ConstDataRange(reinterpret_cast<const char*>($member_name.data()), $member_name.size());',
            member_name=member_name)

    def get_setter_body(self, member_name):
        # type: (unicode) -> unicode
        return common.template_args(
            '$member_name = ${value};',
            member_name=member_name,
            value=self.get_transform_to_storage_type("value"))

    def get_transform_to_getter_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        return common.template_args('makeCDR(${expression});', expression=expression)

    def get_transform_to_storage_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        return common.template_args(
            'std::vector<std::uint8_t>(reinterpret_cast<const uint8_t*>(${expression}.data()), ' +
            'reinterpret_cast<const uint8_t*>(${expression}.data()) + ${expression}.length())',
            expression=expression)


class _CppTypeDelegating(CppTypeBase):
    """Delegates all method calls a nested instance of CppTypeBase. Used to build other classes."""

    def __init__(self, base, field):
        # type: (CppTypeBase, ast.Field) -> None
        self._base = base
        super(_CppTypeDelegating, self).__init__(field)

    def get_type_name(self):
        # type: () -> unicode
        return self._base.get_type_name()

    def get_storage_type(self):
        # type: () -> unicode
        return self._base.get_storage_type()

    def get_getter_setter_type(self):
        # type: () -> unicode
        return self._base.get_getter_setter_type()

    def return_by_reference(self):
        # type: () -> bool
        return self._base.return_by_reference()

    def disable_xvalue(self):
        # type: () -> bool
        return self._base.disable_xvalue()

    def is_view_type(self):
        # type: () -> bool
        return self._base.is_view_type()

    def get_getter_body(self, member_name):
        # type: (unicode) -> unicode
        return self._base.get_getter_body(member_name)

    def get_setter_body(self, member_name):
        # type: (unicode) -> unicode
        return self._base.get_setter_body(member_name)

    def get_transform_to_getter_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        return self._base.get_transform_to_getter_type(expression)

    def get_transform_to_storage_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        return self._base.get_transform_to_storage_type(expression)


class _CppTypeArray(_CppTypeDelegating):
    """C++ Array type for wrapping a base C++ Type information."""

    def __init__(self, base, field):
        # type: (CppTypeBase, ast.Field) -> None
        super(_CppTypeArray, self).__init__(base, field)

    def get_storage_type(self):
        # type: () -> unicode
        return _qualify_array_type(self._base.get_storage_type())

    def get_getter_setter_type(self):
        # type: () -> unicode
        return _qualify_array_type(self._base.get_getter_setter_type())

    def return_by_reference(self):
        # type: () -> bool
        if self._base.is_view_type():
            return False
        return True

    def disable_xvalue(self):
        # type: () -> bool
        return True

    def get_getter_body(self, member_name):
        # type: (unicode) -> unicode
        convert = self.get_transform_to_getter_type(member_name)
        if convert:
            return common.template_args('return ${convert};', convert=convert)
        else:
            return self._base.get_getter_body(member_name)

    def get_setter_body(self, member_name):
        # type: (unicode) -> unicode
        convert = self.get_transform_to_storage_type("value")
        if convert:
            return common.template_args(
                '${member_name} = ${convert};', member_name=member_name, convert=convert)
        else:
            return self._base.get_setter_body(member_name)

    def get_transform_to_getter_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        if self._base.get_storage_type() != self._base.get_getter_setter_type():
            return common.template_args(
                'transformVector($expression)',
                expression=expression, )
        else:
            return None

    def get_transform_to_storage_type(self, expression):
        # type: (unicode) -> Optional[unicode]
        if self._base.get_storage_type() != self._base.get_getter_setter_type():
            return common.template_args(
                'transformVector($expression)',
                expression=expression, )
        else:
            return None


class _CppTypeOptional(_CppTypeDelegating):
    """Base type for Optional C++ Type information which wraps C++ types."""

    def __init__(self, base, field):
        # type: (CppTypeBase, ast.Field) -> None
        super(_CppTypeOptional, self).__init__(base, field)

    def get_storage_type(self):
        # type: () -> unicode
        return _qualify_optional_type(self._base.get_storage_type())

    def get_getter_setter_type(self):
        # type: () -> unicode
        return _qualify_optional_type(self._base.get_getter_setter_type())

    def disable_xvalue(self):
        # type: () -> bool
        return True

    def return_by_reference(self):
        # type: () -> bool
        return False

    def get_getter_body(self, member_name):
        # type: (unicode) -> unicode
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
                """),
                member_name=member_name,
                convert=convert)
        else:
            return common.template_args(
                'return ${param_type}{${member_name}};',
                param_type=self.get_getter_setter_type(),
                member_name=member_name)

    def get_setter_body(self, member_name):
        # type: (unicode) -> unicode
        convert = self._base.get_transform_to_storage_type("value.get()")
        if convert:
            return common.template_args(
                textwrap.dedent("""\
                            if (value.is_initialized()) {
                                ${member_name} = ${convert};
                            } else {
                                ${member_name} = boost::none;
                            }
                            """),
                member_name=member_name,
                convert=convert)
        else:
            return self._base.get_setter_body(member_name)


def get_cpp_type(field):
    # type: (ast.Field) -> CppTypeBase
    # pylint: disable=redefined-variable-type
    """Get the C++ Type information for the given field."""

    cpp_type_info = None  # type: Any

    if field.cpp_type == 'std::string':
        cpp_type_info = _CppTypeView(field, 'std::string', 'StringData')
    elif field.cpp_type == 'std::vector<std::uint8_t>':
        cpp_type_info = _CppTypeVector(field)
    else:
        cpp_type_info = _CppTypeBasic(field)  # pylint: disable=redefined-variable-type

    if field.array:
        cpp_type_info = _CppTypeArray(cpp_type_info, field)

    if field.optional:
        cpp_type_info = _CppTypeOptional(cpp_type_info, field)

    return cpp_type_info


class BsonCppTypeBase(object):
    """Base type for custom C++ support for BSON Types information."""

    __metaclass__ = ABCMeta

    def __init__(self, field):
        # type: (ast.Field) -> None
        """Construct a BsonCppTypeBase."""
        self._field = field

    @abstractmethod
    def gen_deserializer_expression(self, indented_writer, object_instance):
        # type: (writer.IndentedTextWriter, unicode) -> unicode
        """Generate code with the text writer and return an expression to deserialize the type."""
        pass

    @abstractmethod
    def has_serializer(self):
        # type: () -> bool
        """Return True if this class generate a serializer for the given field."""
        pass

    @abstractmethod
    def gen_serializer_expression(self, indented_writer, expression):
        # type: (writer.IndentedTextWriter, unicode) -> unicode
        """Generate code with the text writer and return an expression to serialize the type."""
        pass


class _StringBsonCppTypeBase(BsonCppTypeBase):
    """Custom C++ support for string BSON types."""

    def __init__(self, field):
        # type: (ast.Field) -> None
        super(_StringBsonCppTypeBase, self).__init__(field)

    def gen_deserializer_expression(self, indented_writer, object_instance):
        # type: (writer.IndentedTextWriter, unicode) -> unicode
        return common.template_args(
            '${object_instance}.valueStringData()', object_instance=object_instance)

    def has_serializer(self):
        # type: () -> bool
        return self._field.serializer is not None

    def gen_serializer_expression(self, indented_writer, expression):
        # type: (writer.IndentedTextWriter, unicode) -> unicode
        method_name = writer.get_method_name(self._field.serializer)
        return common.template_args(
            '${expression}.${method_name}()', expression=expression, method_name=method_name)


class _ObjectBsonCppTypeBase(BsonCppTypeBase):
    """Custom C++ support for object BSON types."""

    def __init__(self, field):
        # type: (ast.Field) -> None
        super(_ObjectBsonCppTypeBase, self).__init__(field)

    def gen_deserializer_expression(self, indented_writer, object_instance):
        # type: (writer.IndentedTextWriter, unicode) -> unicode
        if self._field.deserializer:
            # Call a method like: Class::method(const BSONObj& value)
            indented_writer.write_line(
                common.template_args(
                    'const BSONObj localObject = ${object_instance}.Obj();',
                    object_instance=object_instance))
            return "localObject"

        else:
            # Just pass the BSONObj through without trying to parse it.
            return common.template_args('${object_instance}.Obj()', object_instance=object_instance)

    def has_serializer(self):
        # type: () -> bool
        return self._field.serializer is not None

    def gen_serializer_expression(self, indented_writer, expression):
        # type: (writer.IndentedTextWriter, unicode) -> unicode
        method_name = writer.get_method_name(self._field.serializer)
        indented_writer.write_line(
            common.template_args(
                'const BSONObj localObject = ${expression}.${method_name}();',
                expression=expression,
                method_name=method_name))
        return "localObject"


class _BinDataBsonCppTypeBase(BsonCppTypeBase):
    """Custom C++ support for all binData BSON types."""

    def __init__(self, field):
        # type: (ast.Field) -> None
        super(_BinDataBsonCppTypeBase, self).__init__(field)

    def gen_deserializer_expression(self, indented_writer, object_instance):
        # type: (writer.IndentedTextWriter, unicode) -> unicode
        return common.template_args(
            '${object_instance}._binDataVector()', object_instance=object_instance)

    def has_serializer(self):
        # type: () -> bool
        return True

    def gen_serializer_expression(self, indented_writer, expression):
        # type: (writer.IndentedTextWriter, unicode) -> unicode
        if self._field.serializer:
            method_name = writer.get_method_name(self._field.serializer)
            indented_writer.write_line(
                common.template_args(
                    'ConstDataRange tempCDR = ${expression}.${method_name}();',
                    expression=expression,
                    method_name=method_name))
        else:
            indented_writer.write_line(
                common.template_args(
                    'ConstDataRange tempCDR = makeCDR(${expression});', expression=expression))

        return common.template_args(
            'BSONBinData(tempCDR.data(), tempCDR.length(), ${bindata_subtype})',
            bindata_subtype=bson.cpp_bindata_subtype_type_name(self._field.bindata_subtype))


# For some fields, we want to support custom serialization but defer most of the serialization to
# the core BSONElement class. This means that callers need to only process a string, a vector of
# bytes, or a integer, not a BSONElement or BSONObj.
def get_bson_cpp_type(field):
    # type: (ast.Field) -> Optional[BsonCppTypeBase]
    """Get a class that provides custom serialization for the given BSON type."""

    # Does not support list of types
    if len(field.bson_serialization_type) > 1:
        return None

    if field.bson_serialization_type[0] == 'string':
        return _StringBsonCppTypeBase(field)

    if field.bson_serialization_type[0] == 'object':
        return _ObjectBsonCppTypeBase(field)

    if field.bson_serialization_type[0] == 'bindata':
        return _BinDataBsonCppTypeBase(field)

    # Unsupported type
    return None
