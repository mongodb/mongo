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

from __future__ import absolute_import, print_function, unicode_literals

import itertools
from typing import Any, Dict, Iterator, List, Optional, Tuple, Union

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


def parse_array_type(name):
    # type: (unicode) -> unicode
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
    return itertools.chain.from_iterable(
        (_zip_scalar(value, key) for (key, value) in dic.viewitems()))


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

    def _is_duplicate(self, ctxt, location, name, duplicate_class_name):
        # type: (errors.ParserContext, common.SourceLocation, unicode, unicode) -> bool
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
        if not self._is_duplicate(ctxt, command, command.name, "command"):
            self.commands.append(command)

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

    def resolve_field_type(self, ctxt, location, field_name, type_name):
        # type: (errors.ParserContext, common.SourceLocation, unicode, unicode) -> Optional[Union[Command, Enum, Struct, Type]]
        """Find the type or struct a field refers to or log an error."""
        return self._resolve_field_type(ctxt, location, field_name, type_name)

    def _resolve_field_type(self, ctxt, location, field_name, type_name):
        # type: (errors.ParserContext, common.SourceLocation, unicode, unicode) -> Optional[Union[Command, Enum, Struct, Type]]
        """Find the type or struct a field refers to or log an error."""
        # pylint: disable=too-many-return-statements

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

        if type_name.startswith('array<'):
            array_type_name = parse_array_type(type_name)
            if not array_type_name:
                ctxt.add_bad_array_type_name_error(location, field_name, type_name)
                return None

            return self._resolve_field_type(ctxt, location, field_name, array_type_name)

        ctxt.add_unknown_type_error(location, field_name, type_name)

        return None


class Global(common.SourceLocation):
    """
    IDL global object container.

    Not all fields may be populated. If they do not exist in the source document, they are not
    populated.
    """

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Global."""
        self.cpp_namespace = None  # type: unicode
        self.cpp_includes = []  # type: List[unicode]
        self.configs = None  # type: ConfigGlobal

        super(Global, self).__init__(file_name, line, column)


class Import(common.SourceLocation):
    """IDL imports object."""

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct an Imports section."""
        self.imports = []  # type: List[unicode]

        # These are not part of the IDL syntax but are produced by the parser.
        # List of imports with structs.
        self.resolved_imports = []  # type: List[unicode]
        # All imports directly or indirectly included
        self.dependencies = []  # type: List[unicode]

        super(Import, self).__init__(file_name, line, column)


class Type(common.SourceLocation):
    """
    Stores all type information about an IDL type.

    The fields name, description, cpp_type, and bson_serialization_type are required.
    Other fields may be populated. If they do not exist in the source document, they are not
    populated.
    """

    # pylint: disable=too-many-instance-attributes

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Type."""
        self.name = None  # type: unicode
        self.description = None  # type: unicode
        self.cpp_type = None  # type: unicode
        self.bson_serialization_type = None  # type: List[unicode]
        self.bindata_subtype = None  # type: unicode
        self.serializer = None  # type: unicode
        self.deserializer = None  # type: unicode
        self.default = None  # type: unicode

        super(Type, self).__init__(file_name, line, column)


class Validator(common.SourceLocation):
    """
    An instance of a validator for a field.

    The validator must include at least one of the defined validation predicates.
    If more than one is included, they must ALL evaluate to true.
    """

    # pylint: disable=too-many-instance-attributes

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Validator."""
        # Don't lint gt/lt as bad attibute names.
        # pylint: disable=C0103
        self.gt = None  # type: Expression
        self.lt = None  # type: Expression
        self.gte = None  # type: Expression
        self.lte = None  # type: Expression
        self.callback = None  # type: unicode

        super(Validator, self).__init__(file_name, line, column)


class Field(common.SourceLocation):
    """
    An instance of a field in a struct.

    The fields name, and type are required.
    Other fields may be populated. If they do not exist in the source document, they are not
    populated.
    """

    # pylint: disable=too-many-instance-attributes

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Field."""
        self.name = None  # type: unicode
        self.cpp_name = None  # type: unicode
        self.description = None  # type: unicode
        self.type = None  # type: unicode
        self.ignore = False  # type: bool
        self.optional = False  # type: bool
        self.default = None  # type: unicode
        self.supports_doc_sequence = False  # type: bool
        self.comparison_order = -1  # type: int
        self.validator = None  # type: Validator

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
        # type: (unicode, int, int) -> None
        """Construct a Type."""
        self.name = None  # type: unicode
        self.cpp_name = None  # type: unicode

        super(ChainedStruct, self).__init__(file_name, line, column)


class ChainedType(common.SourceLocation):
    """
    Stores all type information about an IDL chained type.

    The fields name, and cpp_name are required.
    """

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Type."""
        self.name = None  # type: unicode
        self.cpp_name = None  # type: unicode

        super(ChainedType, self).__init__(file_name, line, column)


class Struct(common.SourceLocation):
    """
    IDL struct information.

    All fields are either required or have a non-None default.
    """

    # pylint: disable=too-many-instance-attributes

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Struct."""
        self.name = None  # type: unicode
        self.description = None  # type: unicode
        self.strict = True  # type: bool
        self.immutable = False  # type: bool
        self.inline_chained_structs = True  # type: bool
        self.generate_comparison_operators = False  # type: bool
        self.chained_types = None  # type: List[ChainedType]
        self.chained_structs = None  # type: List[ChainedStruct]
        self.fields = None  # type: List[Field]

        # Command only property
        self.cpp_name = None  # type: unicode

        # Internal property that is not represented as syntax. An imported struct is read from an
        # imported file, and no code is generated for it.
        self.imported = False  # type: bool

        # Internal property: cpp_namespace from globals section
        self.cpp_namespace = None  # type: unicode

        super(Struct, self).__init__(file_name, line, column)


class Command(Struct):
    """
    IDL command information, a subtype of Struct.

    Namespace is required.
    """

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Command."""
        self.namespace = None  # type: unicode
        self.type = None  # type: unicode

        super(Command, self).__init__(file_name, line, column)


class EnumValue(common.SourceLocation):
    """
    IDL Enum Value information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct an Enum."""
        self.name = None  # type: unicode
        self.value = None  # type: unicode

        super(EnumValue, self).__init__(file_name, line, column)


class Enum(common.SourceLocation):
    """
    IDL Enum information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct an Enum."""
        self.name = None  # type: unicode
        self.description = None  # type: unicode
        self.type = None  # type: unicode
        self.values = None  # type: List[EnumValue]

        # Internal property that is not represented as syntax. An imported enum is read from an
        # imported file, and no code is generated for it.
        self.imported = False  # type: bool

        # Internal property: cpp_namespace from globals section
        self.cpp_namespace = None  # type: unicode

        super(Enum, self).__init__(file_name, line, column)


class Condition(common.SourceLocation):
    """Condition(s) for a ServerParameter or ConfigOption."""

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Condition."""
        self.expr = None  # type: unicode
        self.constexpr = None  # type: unicode
        self.preprocessor = None  # type: unicode

        super(Condition, self).__init__(file_name, line, column)


class Expression(common.SourceLocation):
    """Description of a valid C++ expression."""

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct an Expression."""

        self.literal = None  # type: unicode
        self.expr = None  # type: unicode
        self.is_constexpr = True  # type: bool

        super(Expression, self).__init__(file_name, line, column)


class ServerParameterClass(common.SourceLocation):
    """ServerParameter as C++ class specialization."""

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a ServerParameterClass."""

        self.name = None  # type: unicode
        self.data = None  # type: unicode
        self.override_ctor = False  # type: bool
        self.override_set = False  # type: bool

        super(ServerParameterClass, self).__init__(file_name, line, column)


class ServerParameter(common.SourceLocation):
    """IDL ServerParameter information."""

    # pylint: disable=too-many-instance-attributes

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a ServerParameter."""
        self.name = None  # type: unicode
        self.set_at = None  # type: List[unicode]
        self.description = None  # type: unicode
        self.cpp_vartype = None  # type: unicode
        self.cpp_varname = None  # type: unicode
        self.cpp_class = None  # type: ServerParameterClass
        self.condition = None  # type: Condition
        self.deprecated_name = []  # type: List[unicode]
        self.redact = False  # type: bool
        self.test_only = False  # type: bool
        self.default = None  # type: Expression

        # Only valid if cpp_varname is specified.
        self.validator = None  # type: Validator
        self.on_update = None  # type: unicode

        super(ServerParameter, self).__init__(file_name, line, column)


class ConfigGlobal(common.SourceLocation):
    """Global values to apply to all ConfigOptions."""

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a ConfigGlobal."""
        self.section = None  # type: unicode
        self.source = []  # type: List[unicode]
        self.initializer_name = None  # type: unicode

        super(ConfigGlobal, self).__init__(file_name, line, column)


class ConfigOption(common.SourceLocation):
    """Runtime configuration setting definition."""

    # pylint: disable=too-many-instance-attributes

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a ConfigOption."""
        self.name = None  # type: unicode
        self.deprecated_name = []  # type: List[unicode]
        self.short_name = None  # type: unicode
        self.single_name = None  # type: unicode
        self.deprecated_short_name = []  # type: List[unicode]

        self.description = None  # type: unicode
        self.section = None  # type: unicode
        self.arg_vartype = None  # type: unicode
        self.cpp_vartype = None  # type: unicode
        self.cpp_varname = None  # type: unicode
        self.condition = None  # type: Condition

        self.conflicts = []  # type: List[unicode]
        self.requires = []  # type: List[unicode]
        self.hidden = False  # type: bool
        self.redact = False  # type: bool
        self.default = None  # type: Expression
        self.implicit = None  # type: Expression
        self.source = []  # type: List[unicode]

        self.duplicate_behavior = None  # type: unicode
        self.positional = None  # type unicode
        self.validator = None  # type: Validator

        super(ConfigOption, self).__init__(file_name, line, column)
