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
"""
BSON Type Information.

Utilities for validating bson types, etc.
"""

from typing import Dict, List

# Dictionary of BSON type Information
# scalar: True if the type is not an array or object
# bson_type_enum: The BSONType enum value for the given type
_BSON_TYPE_INFORMATION = {
    "double": {'scalar': True, 'bson_type_enum': 'NumberDouble'},
    "string": {'scalar': True, 'bson_type_enum': 'String'},
    "object": {'scalar': False, 'bson_type_enum': 'Object'},
    # TODO: add support: "array" : { 'scalar' :  False, 'bson_type_enum' : 'Array'},
    "bindata": {'scalar': True, 'bson_type_enum': 'BinData'},
    "undefined": {'scalar': True, 'bson_type_enum': 'Undefined'},
    "objectid": {'scalar': True, 'bson_type_enum': 'jstOID'},
    "bool": {'scalar': True, 'bson_type_enum': 'Bool'},
    "date": {'scalar': True, 'bson_type_enum': 'Date'},
    "null": {'scalar': True, 'bson_type_enum': 'jstNULL'},
    "regex": {'scalar': True, 'bson_type_enum': 'RegEx'},
    "int": {'scalar': True, 'bson_type_enum': 'NumberInt'},
    "timestamp": {'scalar': True, 'bson_type_enum': 'bsonTimestamp'},
    "long": {'scalar': True, 'bson_type_enum': 'NumberLong'},
    "decimal": {'scalar': True, 'bson_type_enum': 'NumberDecimal'},
}

# Dictionary of BinData subtype type Information
# scalar: True if the type is not an array or object
# bindata_enum: The BinDataType enum value for the given type
_BINDATA_SUBTYPE = {
    "generic": {'scalar': True, 'bindata_enum': 'BinDataGeneral'},
    "function": {'scalar': True, 'bindata_enum': 'Function'},
    # Also simply known as type 2, deprecated, and requires special handling
    #"binary": {
    #    'scalar': False,
    #    'bindata_enum': 'ByteArrayDeprecated'
    #},
    # Deprecated
    # "uuid_old": {
    #     'scalar': False,
    #     'bindata_enum': 'bdtUUID'
    # },
    "uuid": {'scalar': True, 'bindata_enum': 'newUUID'},
    "md5": {'scalar': True, 'bindata_enum': 'MD5Type'},
}


def is_valid_bson_type(name):
    # type: (str) -> bool
    """Return True if this is a valid bson type."""
    return name in _BSON_TYPE_INFORMATION


def is_scalar_bson_type(name):
    # type: (str) -> bool
    """Return True if this bson type is a scalar."""
    assert is_valid_bson_type(name)
    return _BSON_TYPE_INFORMATION[name]['scalar']  # type: ignore


def cpp_bson_type_name(name):
    # type: (str) -> str
    """Return the C++ type name for a bson type."""
    assert is_valid_bson_type(name)
    return _BSON_TYPE_INFORMATION[name]['bson_type_enum']  # type: ignore


def list_valid_types():
    # type: () -> List[str]
    """Return a list of supported bson types."""
    return [a for a in _BSON_TYPE_INFORMATION]


def is_valid_bindata_subtype(name):
    # type: (str) -> bool
    """Return True if this bindata subtype is valid."""
    return name in _BINDATA_SUBTYPE


def cpp_bindata_subtype_type_name(name):
    # type: (str) -> str
    """Return the C++ type name for a bindata subtype."""
    assert is_valid_bindata_subtype(name)
    return _BINDATA_SUBTYPE[name]['bindata_enum']  # type: ignore
