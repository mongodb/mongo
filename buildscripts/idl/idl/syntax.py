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
IDL Parser Syntax classes.

These class represent the structure of the raw IDL document.
It maps 1-1 to the YAML file, and has not been checked if
it follows the rules of the IDL, etc.
"""

from __future__ import absolute_import, print_function, unicode_literals

# from typing import List, Union, Any, Optional, Tuple

from . import common
from . import errors


class IDLParsedSpec(object):
    """A parsed IDL document or a set of errors if parsing failed."""

    def __init__(self, spec, error_collection):
        # type: (IDLSpec, errors.ParserErrorCollection) -> None
        """Must specify either an IDL document or errors, not both."""
        assert (spec is None and error_collection is not None) or (spec is not None and
                                                                   error_collection is None)
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
        #TODO self.imports = None # type: Optional[Imports]


class SymbolTable(object):
    """
    IDL Symbol Table.

    - Contains all information to resolve commands, types, and structs.
    - Checks for duplicate names across the union of (commands, types, structs)
    """

    def __init__(self):
        # type: () -> None
        """Construct an empty symbol table."""
        self.types = []  # type: List[Type]
        self.structs = []  # type: List[Struct]
        self.commands = []  # type: List[Command]

    def _is_duplicate(self, ctxt, location, name, duplicate_class_name):
        # type: (errors.ParserContext, common.SourceLocation, unicode, unicode) -> bool
        """Return true if the given item already exist in the symbol table."""
        for struct in self.structs:
            if struct.name == name:
                ctxt.add_duplicate_symbol_error(location, name, duplicate_class_name, 'struct')
                return True

        for idltype in self.types:
            if idltype.name == name:
                ctxt.add_duplicate_symbol_error(location, name, duplicate_class_name, 'type')
                return True

        return False

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
        """Add an IDL command  to the symbol table and check for duplicates."""
        # TODO: add commands
        pass

    def resolve_field_type(self, ctxt, field):
        # type: (errors.ParserContext, Field) -> Tuple[Optional[Struct], Optional[Type]]
        """Find the type or struct a field refers to or log an error."""
        for idltype in self.types:
            if idltype.name == field.type:
                return (None, idltype)

        for struct in self.structs:
            if struct.name == field.type:
                return (struct, None)

        # TODO: handle array
        ctxt.add_unknown_type_error(field, field.name, field.type)

        return (None, None)


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
        super(Global, self).__init__(file_name, line, column)


# TODO: add support for imports
class Import(common.SourceLocation):
    """IDL imports object."""

    pass


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
        self.description = None  # type: unicode
        self.type = None  # type: unicode
        self.ignore = False  # type: bool
        self.optional = False  # type: bool
        self.default = None  # type: unicode

        super(Field, self).__init__(file_name, line, column)


class Struct(common.SourceLocation):
    """
    IDL struct information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Struct."""
        self.name = None  # type: unicode
        self.description = None  # type: unicode
        self.strict = True  # type: bool
        self.fields = None  # type: List[Field]
        super(Struct, self).__init__(file_name, line, column)


# TODO: add support for commands
class Command(Struct):
    """IDL command."""
