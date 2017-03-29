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
IDL Common classes.

Classes which are shared among both the IDL idl.syntax and idl.AST trees.
"""

from __future__ import absolute_import, print_function, unicode_literals


class SourceLocation(object):
    """Source location information about an idl.syntax or idl.AST object."""

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a source location."""
        self.file_name = file_name
        self.line = line
        self.column = column
