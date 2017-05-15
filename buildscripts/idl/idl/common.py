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

import os
import string
from typing import Mapping

COMMAND_NAMESPACE_CONCATENATE_WITH_DB = "concatenate_with_db"
COMMAND_NAMESPACE_IGNORED = "ignored"


def title_case(name):
    # type: (unicode) -> unicode
    """Return a CapitalCased version of a string."""
    return name[0:1].upper() + name[1:]


def camel_case(name):
    # type: (unicode) -> unicode
    """Return a camelCased version of a string."""
    return name[0:1].lower() + name[1:]


def template_format(template, template_params=None):
    # type: (unicode, Mapping[unicode,unicode]) -> unicode
    """Write a template to the stream."""
    # Ignore the types since we use unicode literals and this expects str but works fine with
    # unicode.
    # See https://docs.python.org/2/library/string.html#template-strings
    return string.Template(template).substitute(template_params)  # type: ignore


def template_args(template, **kwargs):
    # type: (unicode, **unicode) -> unicode
    """Write a template to the stream."""
    # Ignore the types since we use unicode literals and this expects str but works fine with
    # unicode.
    # See https://docs.python.org/2/library/string.html#template-strings
    return string.Template(template).substitute(kwargs)  # type: ignore


class SourceLocation(object):
    """Source location information about an idl.syntax or idl.AST object."""

    def __init__(self, file_name, line, column):
        # type: (unicode, int, int) -> None
        """Construct a source location."""
        self.file_name = file_name
        self.line = line
        self.column = column

    def __str__(self):
        # type: () -> str
        """
        Return a formatted location.

        Example location message:
        test.idl: (17, 4)
        """
        msg = "%s: (%d, %d)" % (os.path.basename(self.file_name), self.line, self.column)
        return msg  # type: ignore
