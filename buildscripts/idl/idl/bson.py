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
"""
BSON Type Information.

Utilities for validating bson types, etc.
"""

from __future__ import absolute_import, print_function, unicode_literals

# from typing import Dict, List

# Dictionary of BSON type Information
# scalar: True if the type is not an array or object
# bson_type_enum: The BSONType enum value for the given type
_BSON_TYPE_INFORMATION = {
    "double": {
        'scalar': True,
        'bson_type_enum': 'NumberDouble'
    },
    "string": {
        'scalar': True,
        'bson_type_enum': 'String'
    },
    "object": {
        'scalar': False,
        'bson_type_enum': 'Object'
    },
    # TODO: add support: "array" : { 'scalar' :  False, 'bson_type_enum' : 'Array'},
    "bindata": {
        'scalar': True,
        'bson_type_enum': 'BinData'
    },
    "undefined": {
        'scalar': True,
        'bson_type_enum': 'Undefined'
    },
    "objectid": {
        'scalar': True,
        'bson_type_enum': 'jstOID'
    },
    "bool": {
        'scalar': True,
        'bson_type_enum': 'Bool'
    },
    "date": {
        'scalar': True,
        'bson_type_enum': 'Date'
    },
    "null": {
        'scalar': True,
        'bson_type_enum': 'jstNULL'
    },
    "regex": {
        'scalar': True,
        'bson_type_enum': 'RegEx'
    },
    "int": {
        'scalar': True,
        'bson_type_enum': 'NumberInt'
    },
    "timestamp": {
        'scalar': True,
        'bson_type_enum': 'bsonTimestamp'
    },
    "long": {
        'scalar': True,
        'bson_type_enum': 'NumberLong'
    },
    "decimal": {
        'scalar': True,
        'bson_type_enum': 'NumberDecimal'
    },
}

# Dictionary of BinData subtype type Information
# scalar: True if the type is not an array or object
# bindata_enum: The BinDataType enum value for the given type
_BINDATA_SUBTYPE = {
    "generic": {
        'scalar': True,
        'bindata_enum': 'BinDataGeneral'
    },
    "function": {
        'scalar': True,
        'bindata_enum': 'Function'
    },
    "binary": {
        'scalar': False,
        'bindata_enum': 'ByteArrayDeprecated'
    },
    "uuid_old": {
        'scalar': False,
        'bindata_enum': 'bdtUUID'
    },
    "uuid": {
        'scalar': True,
        'bindata_enum': 'newUUID'
    },
    "md5": {
        'scalar': True,
        'bindata_enum': 'MD5Type'
    },
}


def is_valid_bson_type(name):
    # type: (unicode) -> bool
    """Return True if this is a valid bson type."""
    return name in _BSON_TYPE_INFORMATION


def is_scalar_bson_type(name):
    # type: (unicode) -> bool
    """Return True if this bson type is a scalar."""
    assert is_valid_bson_type(name)
    return _BSON_TYPE_INFORMATION[name]['scalar']  # type: ignore


def cpp_bson_type_name(name):
    # type: (unicode) -> unicode
    """Return the C++ type name for a bson type."""
    assert is_valid_bson_type(name)
    return _BSON_TYPE_INFORMATION[name]['bson_type_enum']  # type: ignore


def list_valid_types():
    # type: () -> List[unicode]
    """Return a list of supported bson types."""
    return [a for a in _BSON_TYPE_INFORMATION.iterkeys()]


def is_valid_bindata_subtype(name):
    # type: (unicode) -> bool
    """Return True if this bindata subtype is valid."""
    return name in _BINDATA_SUBTYPE


def cpp_bindata_subtype_type_name(name):
    # type: (unicode) -> unicode
    """Return the C++ type name for a bindata subtype."""
    assert is_valid_bindata_subtype(name)
    return _BINDATA_SUBTYPE[name]['bindata_enum']  # type: ignore
