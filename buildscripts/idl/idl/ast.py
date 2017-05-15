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
IDL AST classes.

Represents the derived IDL specification after type resolution in the binding pass has occurred.

This is a lossy translation from the IDL Syntax tree as the IDL AST only contains information about
the enums and structs that need code generated for them, and just enough information to do that.
"""

from __future__ import absolute_import, print_function, unicode_literals

from typing import List, Union, Any, Optional, Tuple

from . import common
from . import errors


class IDLBoundSpec(object):
    """A bound IDL document or a set of errors if parsing failed."""

    def __init__(self, spec, error_collection):
        # type: (IDLAST, errors.ParserErrorCollection) -> None
        """Must specify either an IDL document or errors, not both."""
        assert (spec is None and error_collection is not None) or (spec is not None and
                                                                   error_collection is None)
        self.spec = spec
        self.errors = error_collection


class IDLAST(object):
    """The in-memory representation of an IDL file."""

    def __init__(self):
        # type: () -> None
        """Construct an IDLAST."""
        self.globals = None  # type: Global

        self.commands = []  # type: List[Command]
        self.enums = []  # type: List[Enum]
        self.structs = []  # type: List[Struct]


class Global(common.SourceLocation):
    """
    IDL global object container.

    cpp_namespace and cpp_includes are only populated if the IDL document contains these YAML nodes.
    """

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Global."""
        self.cpp_namespace = None  # type: unicode
        self.cpp_includes = []  # type: List[unicode]
        super(Global, self).__init__(file_name, line, column)


class Struct(common.SourceLocation):
    """
    IDL struct information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a struct."""
        self.name = None  # type: unicode
        self.description = None  # type: unicode
        self.strict = True  # type: bool
        self.chained_types = []  # type: List[Field]
        self.fields = []  # type: List[Field]
        super(Struct, self).__init__(file_name, line, column)


class Field(common.SourceLocation):
    """
    An instance of a field in a struct.

    Name is always populated.
    A field will either have a struct_type or a cpp_type, but not both.
    Not all fields are set, it depends on the input document.
    """

    # pylint: disable=too-many-instance-attributes

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a Field."""
        self.name = None  # type: unicode
        self.description = None  # type: unicode
        self.cpp_name = None  # type: unicode
        self.optional = False  # type: bool
        self.ignore = False  # type: bool
        self.chained = False  # type: bool

        # Properties specific to fields which are types.
        self.cpp_type = None  # type: unicode
        self.bson_serialization_type = None  # type: List[unicode]
        self.serializer = None  # type: unicode
        self.deserializer = None  # type: unicode
        self.bindata_subtype = None  # type: unicode
        self.default = None  # type: unicode

        # Properties specific to fields which are structs.
        self.struct_type = None  # type: unicode

        # Properties specific to fields which are arrays.
        self.array = False  # type: bool

        # Properties specific to fields which are enums.
        self.enum_type = False  # type: bool

        super(Field, self).__init__(file_name, line, column)


class Command(Struct):
    """
    IDL commmand information.

    All fields are either required or have a non-None default.
    """

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a command."""
        self.namespace = None  # type: unicode
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
        self.values = []  # type: List[EnumValue]

        super(Enum, self).__init__(file_name, line, column)
