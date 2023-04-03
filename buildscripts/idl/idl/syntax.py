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
IDL Parser Syntax classes.

These class represent the structure of the raw IDL document.
It maps 1-1 to the YAML file, and has not been checked if
it follows the rules of the IDL, etc.
"""

import itertools
from typing import Any, Dict, Iterator, List, Optional, Tuple, Union, cast

from . import common
from . import errors


class IDLParsedSpec(object):
    """A parsed IDL document or a set of errors if parsing failed."""

    def __init__(self, spec, error_collection):
        # type: (IDLSpec, errors.ParserErrorCollection) -> None
        """Must specify either an IDL document or errors, not both."""
        assert (spec is None and error_collection is not None) or (spec is not None
                                                                   and error_collection is None)
        self.spec = spec
        self.errors = error_collection


class IDLSpec(object):
    """
    The in-memory representation of an IDL file.

    - Includes all imported files.
    """

    def __init__(self):
        # type: () -> None
        """Construct an IDL spec."""
        self.symbols = SymbolTable()  # type: SymbolTable
        self.globals = None  # type: Optional[Global]
        self.imports = None  # type: Optional[Import]
        self.server_parameters = []  # type: List[ServerParameter]
        self.configs = []  # type: List[ConfigOption]
        self.feature_flags = []  # type: List[FeatureFlag]


def parse_array_variant_types(name):
    # type: (str) -> List[str]
    """Parse a type name of the form 'array<variant<type1, type2, ...>>' and extract types."""
    if not name.startswith("array<variant<") and not name.endswith(">>"):
        return None

    name = name[len("array<variant<"):]
    name = name[:-2]

    variant_types = []
    for variant_type in name.split(','):
        variant_type = variant_type.strip()
        # Ban array<variant<..., array<...>, ...>> types.
        if variant_type.startswith("array<") and variant_type.endswith(">"):
            return None
        variant_types.append(variant_type)

    return variant_types


def parse_array_type(name):
    # type: (str) -> str
    """Parse a type name of the form 'array<type>' and extract type."""
    if not name.startswith("array<") and not name.endswith(">"):
        return None

    name = name[len("array<"):]
    name = name[:-1]

    # V1 restriction, ban nested array types to reduce scope.
    if name.startswith("array<") and name.endswith(">"):
        return None

    return name


def _zip_scalar(items, obj):
    # type: (List[Any], Any) -> Iterator[Tuple[Any, Any]]
    """Return an Iterator of (obj, list item) tuples."""
    return ((item, obj) for item in items)


def _item_and_type(dic):
    # type: (Dict[Any, List[Any]]) -> Iterator[Tuple[Any, Any]]
    """Return an Iterator of (key, value) pairs from a dictionary."""
    return itertools.chain.from_iterable((_zip_scalar(value, key) for (key, value) in dic.items()))


class SymbolTable(object):
    """
    IDL Symbol Table.

    - Contains all information to resolve commands, enums, structs, types, and server parameters.
    - Checks for duplicate names across the union of (commands, enums, types, structs)
    """

    def __init__(self):
        # type: () -> None
        """Construct an empty symbol table."""
        self.commands = []  # type: List[Command]
        self.enums = []  # type: List[Enum]
        self.structs = []  # type: List[Struct]
        self.types = []  # type: List[Type]
        self.generic_argument_lists = []  # type: List[Struct]
        self.generic_reply_field_lists = []  # type: List[Struct]

    def _is_duplicate(self, ctxt, location, name, duplicate_class_name):
        # type: (errors.ParserContext, common.SourceLocation, str, str) -> bool
        """Return true if the given item already exist in the symbol table."""
        for (item, entity_type) in _item_and_type({
                "command": self.commands,
                "enum": self.enums,
                "struct": self.structs,
                "type": self.types,
        }):
            if item.name == name:
                ctxt.add_duplicate_symbol_error(location, name, duplicate_class_name, entity_type)
                return True
            if entity_type == "command":
                if name in [item.command_name, item.command_alias if item.command_alias else '']:
                    ctxt.add_duplicate_symbol_error(location, name, duplicate_class_name,
                                                    entity_type)
                    return True

        return False

    def add_enum(self, ctxt, idl_enum):
        # type: (errors.ParserContext, Enum) -> None
        """Add an IDL enum to the symbol table and check for duplicates."""
        if not self._is_duplicate(ctxt, idl_enum, idl_enum.name, "enum"):
            self.enums.append(idl_enum)

    def add_struct(self, ctxt, struct):
        # type: (errors.ParserContext, Struct) -> None
        """Add an IDL struct to the symbol table and check for duplicates."""
        if not self._is_duplicate(ctxt, struct, struct.name, "struct"):
            self.structs.append(struct)

    def add_type(self, ctxt, idltype):
        # type: (errors.ParserContext, Type) -> None
        """Add an IDL type to the symbol table and check for duplicates."""
        if not self._is_duplicate(ctxt, idltype, idltype.name, "type"):
            self.types.append(idltype)

    def add_command(self, ctxt, command):
        # type: (errors.ParserContext, Command) -> None
        """Add an IDL command to the symbol table and check for duplicates."""
        if (not self._is_duplicate(ctxt, command, command.name, "command")
                and not self._is_duplicate(ctxt, command, command.command_alias, "command")):
            self.commands.append(command)

    def add_generic_argument_list(self, field_list):
        # type: (Struct) -> None
        """Add an IDL generic argument list to the symbol table."""
        self.generic_argument_lists.append(field_list)

    def add_generic_reply_field_list(self, field_list):
        # type: (Struct) -> None
        """Add an IDL generic reply field list to the symbol table."""
        self.generic_reply_field_lists.append(field_list)

    def add_imported_symbol_table(self, ctxt, imported_symbols):
        # type: (errors.ParserContext, SymbolTable) -> None
        """
        Merge all the symbols in the imported_symbols symbol table into the symbol table.

        Marks imported structs as imported, and errors on duplicate symbols.
        """
        for command in imported_symbols.commands:
            if not self._is_duplicate(ctxt, command, command.name, "command"):
                command.imported = True
                self.commands.append(command)

        for struct in imported_symbols.structs:
            if not self._is_duplicate(ctxt, struct, struct.name, "struct"):
                struct.imported = True
                self.structs.append(struct)

        for idl_enum in imported_symbols.enums:
            if not self._is_duplicate(ctxt, idl_enum, idl_enum.name, "enum"):
                idl_enum.imported = True
                self.enums.append(idl_enum)

        for idltype in imported_symbols.types:
            self.add_type(ctxt, idltype)

    def get_struct(self, name):
        # type: (str) -> Struct
        """Get the struct from the SymbolTable's struct list based on the struct name."""
        for struct in self.structs:
            if struct.name == name:
                return struct
        return None

    def get_generic_argument_list(self, name):
        # type: (str) -> Struct
        """Get a generic argument list from the SymbolTable based on the list name."""
        for gen_arg_list in self.generic_argument_lists:
            if gen_arg_list.name == name:
                return gen_arg_list
        return None

    def get_generic_reply_field_list(self, name):
        # type: (str) -> Struct
        """Get a generic reply field list from the SymbolTable based on the list name."""
        for gen_reply_field_list in self.generic_reply_field_lists:
            if gen_reply_field_list.name == name:
                return gen_reply_field_list
        return None

    def resolve_type_from_name(self, ctxt, location, field_name, field_type_name):
        # type: (errors.ParserContext, common.SourceLocation, str, str) -> Optional[Union[Enum, Struct, Type]]
        """Find the type or struct a field refers to or log an error."""
        field_type = FieldTypeSingle(location.file_name, location.line, location.column)
        field_type.type_name = field_type_name
        return self.resolve_field_type(ctxt, location, field_name, field_type)

    def resolve_field_type(self, ctxt, location, field_name, field_type):
        # type: (errors.ParserContext, common.SourceLocation, str, FieldType) -> Optional[Union[Enum, Struct, Type]]
        """Find the type or struct a field refers to or log an error."""

        if isinstance(field_type, FieldTypeVariant):
            variant = VariantType(field_type.file_name, field_type.line, field_type.column)
            variant.bson_serialization_type = []
            for alternative in field_type.variant:
                alternative_type = self.resolve_field_type(ctxt, location, field_name, alternative)
                if not alternative_type:
                    # There was an error.
                    return None

                if isinstance(alternative_type, Enum):
                    ctxt.add_variant_enum_error(location, field_name, alternative_type.name)
                    return None

                if isinstance(alternative_type, Struct):
                    if len(variant.variant_struct_types) > 0:
                        # Check if we are adding a duplicate first field name since that would
                        # cause parsing ambiguity.
                        first_element = alternative_type.fields[0].name
                        if first_element in [
                                elem.fields[0].name for elem in variant.variant_struct_types
                        ]:
                            ctxt.add_variant_structs_error(location, field_name)
                            continue
                    variant.variant_struct_types.append(alternative_type)
                    bson_serialization_type = ["object"]
                else:
                    variant.variant_types.append(alternative_type)
                    if isinstance(alternative_type, ArrayType):
                        base_type = cast(Type, alternative_type.element_type)
                    else:
                        base_type = cast(Type, alternative_type)

                    bson_serialization_type = []
                    # If alternative_type is an array, element type could be Struct or Type.
                    if isinstance(base_type, Type):
                        bson_serialization_type = cast(Type, base_type).bson_serialization_type

                variant.bson_serialization_type.extend(bson_serialization_type)

            return variant

        if isinstance(field_type, FieldTypeArray):
            element_type = self.resolve_field_type(ctxt, location, field_name,
                                                   field_type.element_type)
            if not element_type:
                ctxt.add_unknown_type_error(location, field_name,
                                            field_type.element_type.debug_string())
                return None

            if isinstance(element_type, Enum):
                ctxt.add_array_enum_error(location, field_name)
                return None

            return ArrayType(element_type)

        assert isinstance(field_type, FieldTypeSingle)
        type_name = field_type.type_name
        if type_name.startswith('array<'):
            # The caller should've already stripped "array<...>" from type_name, this may be an
            # illegal nested array like "array<array<...>>".
            ctxt.add_bad_array_type_name_error(location, field_name, type_name)
            return None

        for command in self.commands:
            if command.name == type_name:
                return command

        for idl_enum in self.enums:
            if idl_enum.name == type_name:
                return idl_enum

        for struct in self.structs:
            if struct.name == type_name:
                return struct

        for idltype in self.types:
            if idltype.name == type_name:
                return idltype

        ctxt.add_unknown_type_error(location, field_name, type_name)

        return None


class Global(common.SourceLocation):
    """
    IDL global object container.

    Not all fields may be populated. If they do not exist in the source document, they are not
    populated.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Global."""
        self.cpp_namespace = None  # type: str
        self.cpp_includes = []  # type: List[str]
        self.configs = None  # type: ConfigGlobal

        super(Global, self).__init__(file_name, line, column)


class Import(common.SourceLocation):
    """IDL imports object."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an Imports section."""
        self.imports = []  # type: List[str]

        # These are not part of the IDL syntax but are produced by the parser.
        # List of imports with structs.
        self.resolved_imports = []  # type: List[str]
        # All imports directly or indirectly included
        self.dependencies = []  # type: List[str]

        super(Import, self).__init__(file_name, line, column)


class Type(common.SourceLocation):
    """
    Stores all type information about an IDL type.

    The fields name, description, cpp_type, and bson_serialization_type are required.
    Other fields may be populated. If they do not exist in the source document, they are not
    populated.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Type."""
        self.name = None  # type: str
        self.cpp_type = None  # type: str
        self.bson_serialization_type = None  # type: List[str]
        self.bindata_subtype = None  # type: str
        self.serializer = None  # type: str
        self.deserializer = None  # type: str
        self.description = None  # type: str
        self.deserialize_with_tenant = False  # type: bool
        self.default = None  # type: str
        self.internal_only = False  # type: bool

        super(Type, self).__init__(file_name, line, column)


class ArrayType(Type):
    """Stores all type information about an IDL array type."""

    def __init__(self, element_type):
        # type: (Union[Struct, Type]) -> None
        """Construct an ArrayType."""
        super(ArrayType, self).__init__(element_type.file_name, element_type.line,
                                        element_type.column)
        self.name = f'array<{element_type.name}>'
        self.element_type = element_type
        if isinstance(element_type, Type):
            if element_type.cpp_type:
                self.cpp_type = f'std::vector<{element_type.cpp_type}>'
            else:
                assert isinstance(element_type, VariantType)
                # cpp_type can't be set here for array of variants as element_type.cpp_type is not set yet.


class VariantType(Type):
    """Stores all type information about an IDL variant type."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a VariantType."""
        super(VariantType, self).__init__(file_name, line, column)
        self.name = 'variant'
        self.variant_types = []  # type: List[Type]
        self.variant_struct_types = []  # type: List[Struct]


class Validator(common.SourceLocation):
    """
    An instance of a validator for a field.

    The validator must include at least one of the defined validation predicates.
    If more than one is included, they must ALL evaluate to true.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Validator."""
        # Don't lint gt/lt as bad attibute names.
        # pylint: disable=C0103
        self.gt = None  # type: Expression
        self.lt = None  # type: Expression
        self.gte = None  # type: Expression
        self.lte = None  # type: Expression
        self.callback = None  # type: str

        super(Validator, self).__init__(file_name, line, column)

    def __eq__(self, other):
        return (isinstance(other, Validator) and self.gt == other.gt and self.lt == other.lt
                and self.gte == other.gte and self.lte == other.lte
                and self.callback == other.callback)

    def __ne__(self, other):
        return not self == other

    def __hash__(self):
        return hash((self.gt, self.lt, self.gte, self.lte, self.callback))


class Field(common.SourceLocation):
    """
    An instance of a field in a struct.

    The fields name, and type are required.
    Other fields may be populated. If they do not exist in the source document, they are not
    populated.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Field."""
        self.name = None  # type: str
        self.cpp_name = None  # type: str
        self.description = None  # type: str
        self.type = None  # type: FieldType
        self.ignore = False  # type: bool
        self.optional = False  # type: bool
        self.default = None  # type: str
        self.supports_doc_sequence = False  # type: bool
        self.comparison_order = -1  # type: int
        self.validator = None  # type: Validator
        self.non_const_getter = False  # type: bool
        self.unstable = None  # type: Optional[bool]
        self.stability = None  # type: Optional[str]
        self.always_serialize = False  # type: bool
        self.forward_to_shards = None  # type: Optional[bool]
        self.forward_from_shards = None  # type: Optional[bool]
        self.preparse = False  # type: bool

        # Internal fields - not generated by parser
        self.serialize_op_msg_request_only = False  # type: bool
        self.constructed = False  # type: bool

        super(Field, self).__init__(file_name, line, column)


class ChainedStruct(common.SourceLocation):
    """
    Stores all type information about an IDL chained struct.

    The fields name, and cpp_name are required.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Type."""
        self.name = None  # type: str
        self.cpp_name = None  # type: str

        super(ChainedStruct, self).__init__(file_name, line, column)


class ChainedType(common.SourceLocation):
    """
    Stores all type information about an IDL chained type.

    The fields name, and cpp_name are required.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Type."""
        self.name = None  # type: str
        self.cpp_name = None  # type: str

        super(ChainedType, self).__init__(file_name, line, column)


class Struct(common.SourceLocation):
    """
    IDL struct information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Struct."""
        self.name = None  # type: str
        self.description = None  # type: str
        self.strict = True  # type: bool
        self.immutable = False  # type: bool
        self.inline_chained_structs = True  # type: bool
        self.generate_comparison_operators = False  # type: bool
        self.chained_types = None  # type: List[ChainedType]
        self.chained_structs = None  # type: List[ChainedStruct]
        self.fields = None  # type: List[Field]
        self.allow_global_collection_name = False  # type: bool
        self.non_const_getter = False  # type: bool
        self.cpp_validator_func = None  # type: str
        self.is_command_reply = False  # type: bool
        self.is_generic_cmd_list = None  # type: Optional[str]

        # Command only property
        self.cpp_name = None  # type: str

        # Internal property that is not represented as syntax. An imported struct is read from an
        # imported file, and no code is generated for it.
        self.imported = False  # type: bool

        # Internal property: cpp_namespace from globals section
        self.cpp_namespace = None  # type: str

        super(Struct, self).__init__(file_name, line, column)


class Privilege(common.SourceLocation):
    """IDL privilege information."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an Privilege."""

        self.resource_pattern = None  # type: str
        self.action_type = None  # type: List[str]
        # This is a parser only field so users can add new agg stages.
        self.agg_stage = None  # type: str

        super(Privilege, self).__init__(file_name, line, column)

    def __str__(self):
        # type: () -> str
        """
        Return formatted privilege information.

        Example privilege message:
        location: test.idl: (17, 4), resource_pattern: exact_namespace, action_type: ['find', 'insert', 'update', 'remove'], agg_stage: None
        """
        location = super(Privilege, self).__str__()
        msg = "location: %s, resource_pattern: %s, action_type: %s, agg_stage: %s" % (
            location, self.resource_pattern, self.action_type, self.agg_stage)
        return msg  # type: ignore


class AccessCheck(common.SourceLocation):
    """IDL access check information."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an AccessCheck."""

        self.check = None  # type: str
        self.privilege = None  # type: Privilege

        super(AccessCheck, self).__init__(file_name, line, column)

    def __str__(self):
        # type: () -> str
        """
        Return formatted access check information.

        Example access check message:
        location: test.idl: (17, 4), check: get_single_user, privilege: (location: test.idl: (18, 6), resource_pattern: exact_namespace, action_type: ['find', 'insert', 'update', 'remove'], agg_stage: None
        """
        location = super(AccessCheck, self).__str__()
        msg = "location: %s, check: %s, privilege: %s" % (location, self.check, self.privilege)
        return msg  # type: ignore


class AccessChecks(common.SourceLocation):
    """IDL access checks information."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an AccessChecks."""

        self.ignore = None  # type: bool
        self.none = None  # type: bool
        self.simple = None  # type: AccessCheck
        self.complex = None  # type: List[AccessCheck]

        super(AccessChecks, self).__init__(file_name, line, column)

    def get_access_check_type(self) -> str:
        """Get type of AccessChecks."""
        if self.ignore:
            return "ignore"
        if self.none:
            return "none"
        if self.simple:
            return "simple"
        if self.complex:
            return "complex"
        return "undefined"


class Command(Struct):
    """
    IDL command information, a subtype of Struct.

    Namespace is required.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Command."""
        self.namespace = None  # type: str
        self.command_name = None  # type: str
        self.command_alias = None  # type: str
        self.type = None  # type: FieldType
        self.reply_type = None  # type: str
        self.api_version = None  # type: str
        self.is_deprecated = False  # type: bool
        self.access_check = None  # type: AccessChecks
        super(Command, self).__init__(file_name, line, column)


class FieldListEntry(common.SourceLocation):
    """Options for a field in a generic argument or generic reply field list."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a FieldListEntry."""
        self.name = None  # type: str
        self.forward_to_shards = False  # type: bool
        self.forward_from_shards = False  # type: bool
        super(FieldListEntry, self).__init__(file_name, line, column)


class EnumValue(common.SourceLocation):
    """
    IDL Enum Value information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an Enum."""
        self.name = None  # type: str
        self.description = None  # type: str
        self.value = None  # type: str
        self.extra_data = None  # type: Dict[str, Any]

        super(EnumValue, self).__init__(file_name, line, column)

    def __eq__(self, other):
        return (isinstance(other, EnumValue) and self.name == other.name
                and self.value == other.value)

    def __ne__(self, other):
        return not self == other

    def __hash__(self):
        return hash((self.name, self.value))


class Enum(common.SourceLocation):
    """
    IDL Enum information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an Enum."""
        self.name = None  # type: str
        self.description = None  # type: str
        self.type = None  # type: str
        self.values = None  # type: List[EnumValue]

        # Internal property that is not represented as syntax. An imported enum is read from an
        # imported file, and no code is generated for it.
        self.imported = False  # type: bool

        # Internal property: cpp_namespace from globals section
        self.cpp_namespace = None  # type: str

        super(Enum, self).__init__(file_name, line, column)


class Condition(common.SourceLocation):
    """Condition(s) for a ServerParameter or ConfigOption."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a Condition."""
        self.expr = None  # type: str
        self.constexpr = None  # type: str
        self.preprocessor = None  # type: str
        self.feature_flag = None  # type: str
        self.min_fcv = None  # type: str

        super(Condition, self).__init__(file_name, line, column)


class FieldType(common.SourceLocation):
    """A field's type, before it is resolved to a Type instance."""

    def debug_string(self):
        """Display this field type in error messages."""
        raise NotImplementedError


class FieldTypeSingle(FieldType):
    """A scalar field's type, before it is resolved to a Type instance."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a FieldTypeSingle."""
        self.type_name = None  # type: str

        super(FieldTypeSingle, self).__init__(file_name, line, column)

    def debug_string(self):
        """Display this field type in error messages."""
        return self.type_name


class FieldTypeArray(FieldType):
    """An array field's type, before it is resolved to a Type instance."""

    def __init__(self, element_type):
        # type: (Union[FieldTypeSingle, FieldTypeVariant]) -> None
        """Construct a FieldTypeArray."""
        self.element_type = element_type  # type: Union[FieldTypeSingle, FieldTypeVariant]

        super(FieldTypeArray, self).__init__(element_type.file_name, element_type.line,
                                             element_type.column)

    def debug_string(self):
        """Display this field type in error messages."""
        return f'array<{self.element_type.type_name}>'


class FieldTypeVariant(FieldType):
    """A variant field's type, before it is resolved to a Type instance."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a FieldTypeVariant."""
        self.variant = []  # type: List[FieldType]

        super(FieldTypeVariant, self).__init__(file_name, line, column)

    def debug_string(self):
        """Display this field type in error messages."""
        return 'variant<%s>' % (', '.join(v.debug_string() for v in self.variant))


class Expression(common.SourceLocation):
    """Description of a valid C++ expression."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct an Expression."""

        self.literal = None  # type: str
        self.expr = None  # type: str
        self.is_constexpr = True  # type: bool

        super(Expression, self).__init__(file_name, line, column)

    def __eq__(self, other):
        return (isinstance(other, Expression) and self.literal == other.literal
                and self.expr == other.expr and self.is_constexpr == other.is_constexpr)

    def __ne__(self, other):
        return not self == other

    def __hash__(self):
        return hash((self.literal, self.expr, self.is_constexpr))


class ServerParameterClass(common.SourceLocation):
    """ServerParameter as C++ class specialization."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a ServerParameterClass."""

        self.name = None  # type: str
        self.data = None  # type: str
        self.override_ctor = False  # type: bool
        self.override_set = False  # type: bool
        self.override_validate = False  # type: bool

        super(ServerParameterClass, self).__init__(file_name, line, column)


class ServerParameter(common.SourceLocation):
    """IDL ServerParameter information."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a ServerParameter."""
        self.name = None  # type: str
        self.set_at = None  # type: List[str]
        self.description = None  # type: str
        self.cpp_vartype = None  # type: str
        self.cpp_varname = None  # type: str
        self.cpp_class = None  # type: ServerParameterClass
        self.condition = None  # type: Condition
        self.deprecated_name = []  # type: List[str]
        self.redact = False  # type: bool
        self.test_only = False  # type: bool
        self.default = None  # type: Expression

        # Only valid if cpp_varname is specified.
        self.validator = None  # type: Validator
        self.on_update = None  # type: str

        super(ServerParameter, self).__init__(file_name, line, column)


class FeatureFlag(common.SourceLocation):
    """IDL FeatureFlag information."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a FeatureFlag."""
        self.name = None  # type: str
        self.description = None  # type: str
        self.cpp_varname = None  # type: str
        self.default = None  # type: Expression
        self.version = None  # type: str

        super(FeatureFlag, self).__init__(file_name, line, column)


class GlobalInitializer(common.SourceLocation):
    """Initializer details for custom registration/storage."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a GlobalInitializer."""

        self.name = None  # type: str
        self.register = None  # type: str
        self.store = None  # type: str

        super(GlobalInitializer, self).__init__(file_name, line, column)


class ConfigGlobal(common.SourceLocation):
    """Global values to apply to all ConfigOptions."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a ConfigGlobal."""
        self.section = None  # type: str
        self.source = []  # type: List[str]
        self.initializer = None  # type: GlobalInitializer

        super(ConfigGlobal, self).__init__(file_name, line, column)


class ConfigOption(common.SourceLocation):
    """Runtime configuration setting definition."""

    def __init__(self, file_name, line, column):
        # type: (str, int, int) -> None
        """Construct a ConfigOption."""
        self.name = None  # type: str
        self.deprecated_name = []  # type: List[str]
        self.short_name = None  # type: str
        self.single_name = None  # type: str
        self.deprecated_short_name = []  # type: List[str]

        self.description = None  # type: Expression
        self.section = None  # type: str
        self.arg_vartype = None  # type: str
        self.cpp_vartype = None  # type: str
        self.cpp_varname = None  # type: str
        self.condition = None  # type: Condition

        self.conflicts = []  # type: List[str]
        self.requires = []  # type: List[str]
        self.hidden = False  # type: bool
        self.redact = False  # type: bool
        self.default = None  # type: Expression
        self.implicit = None  # type: Expression
        self.source = []  # type: List[str]
        self.canonicalize = None  # type: str

        self.duplicate_behavior = None  # type: str
        self.positional = None  # type: str
        self.validator = None  # type: Validator

        super(ConfigOption, self).__init__(file_name, line, column)
