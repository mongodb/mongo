# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""
BSON Type Information.

Utilities for validating bson types, etc.
"""

# Dictionary of BSON type Information
# scalar: True if the type is not an array or object
# bson_type_enum: The BSONType enum value for the given type
_BSON_TYPE_INFORMATION = {
    "double": {"scalar": True, "bson_type_enum": "numberDouble"},
    "string": {"scalar": True, "bson_type_enum": "string"},
    "object": {"scalar": False, "bson_type_enum": "object"},
    "array": {"scalar": False, "bson_type_enum": "array"},
    "bindata": {"scalar": True, "bson_type_enum": "binData"},
    "undefined": {"scalar": True, "bson_type_enum": "undefined"},
    "objectid": {"scalar": True, "bson_type_enum": "oid"},
    "bool": {"scalar": True, "bson_type_enum": "boolean"},
    "date": {"scalar": True, "bson_type_enum": "date"},
    "null": {"scalar": True, "bson_type_enum": "null"},
    "regex": {"scalar": True, "bson_type_enum": "regEx"},
    "int": {"scalar": True, "bson_type_enum": "numberInt"},
    "timestamp": {"scalar": True, "bson_type_enum": "timestamp"},
    "long": {"scalar": True, "bson_type_enum": "numberLong"},
    "decimal": {"scalar": True, "bson_type_enum": "numberDecimal"},
}

# Dictionary of BinData subtype type Information
# scalar: True if the type is not an array or object
# bindata_enum: The BinDataType enum value for the given type
_BINDATA_SUBTYPE = {
    "generic": {"scalar": True, "bindata_enum": "BinDataGeneral"},
    "function": {"scalar": True, "bindata_enum": "Function"},
    # Also simply known as type 2, deprecated, and requires special handling
    # "binary": {
    #    'scalar': False,
    #    'bindata_enum': 'ByteArrayDeprecated'
    # },
    # Deprecated
    # "uuid_old": {
    #     'scalar': False,
    #     'bindata_enum': 'bdtUUID'
    # },
    "uuid": {"scalar": True, "bindata_enum": "newUUID"},
    "md5": {"scalar": True, "bindata_enum": "MD5Type"},
    "encrypt": {"scalar": True, "bindata_enum": "Encrypt"},
    "sensitive": {"scalar": True, "bindata_enum": "Sensitive"},
    "vector": {"scalar": True, "bindata_enum": "Vector"},
}


def is_valid_bson_type(name):
    # type: (str) -> bool
    """Return True if this is a valid bson type."""
    return name in _BSON_TYPE_INFORMATION


def is_scalar_bson_type(name):
    # type: (str) -> bool
    """Return True if this bson type is a scalar."""
    assert is_valid_bson_type(name)
    return _BSON_TYPE_INFORMATION[name]["scalar"]  # type: ignore


def cpp_bson_type_name(name):
    # type: (str) -> str
    """Return the C++ type name for a bson type."""
    assert is_valid_bson_type(name)
    return "::mongo::BSONType::{}".format(_BSON_TYPE_INFORMATION[name]["bson_type_enum"])  # type: ignore


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
    return _BINDATA_SUBTYPE[name]["bindata_enum"]  # type: ignore
