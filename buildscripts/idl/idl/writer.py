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
"""Text Writing Utilites."""

from __future__ import absolute_import, print_function, unicode_literals

import io
import string
from typing import List, Mapping, Union

from . import common

# Number of spaces to indent code
_INDENT_SPACE_COUNT = 4


def _fill_spaces(count):
    # type: (int) -> unicode
    """Fill a string full of spaces."""
    fill = ''
    for _ in range(count * _INDENT_SPACE_COUNT):
        fill += ' '

    return fill


def _indent_text(count, unindented_text):
    # type: (int, unicode) -> unicode
    """Indent each line of a multi-line string."""
    lines = unindented_text.splitlines()
    fill = _fill_spaces(count)
    return '\n'.join(fill + line for line in lines)


def get_method_name(name):
    # type: (unicode) -> unicode
    """Get a method name from a fully qualified method name."""
    pos = name.rfind('::')
    if pos == -1:
        return name
    return name[pos + 2:]


def get_method_name_from_qualified_method_name(name):
    # type: (unicode) -> unicode
    # pylint: disable=invalid-name
    """Get a method name from a fully qualified method name."""
    # TODO: in the future, we may want to support full-qualified calls to static methods
    prefix = 'mongo::'
    pos = name.find(prefix)
    if pos == -1:
        return name

    return name[len(prefix):]


class IndentedTextWriter(object):
    """
    A simple class to manage writing indented lines of text.

    Supports both writing indented lines, and unindented lines.
    Use write_empty_line() instead of write_line('') to avoid lines
    full of blank spaces.
    """

    def __init__(self, stream):
        # type: (io.StringIO) -> None
        """Create an indented text writer."""
        self._stream = stream
        self._indent = 0
        self._template_context = None  # type: Mapping[unicode, unicode]

    def write_unindented_line(self, msg):
        # type: (unicode) -> None
        """Write an unindented line to the stream, no template formattin applied."""
        self._stream.write(msg)
        self._stream.write("\n")

    def indent(self):
        # type: () -> None
        """Indent the text by one level."""
        self._indent += 1

    def unindent(self):
        # type: () -> None
        """Unindent the text by one level."""
        assert self._indent > 0
        self._indent -= 1

    def write_line(self, msg):
        # type: (unicode) -> None
        """Write a line to the stream, no template formattin applied."""
        self._stream.write(_indent_text(self._indent, msg))
        self._stream.write("\n")

    def set_template_mapping(self, template_params):
        # type: (Mapping[unicode,unicode]) -> None
        """Set the current template mapping parameters for string.Template formatting."""
        assert not self._template_context
        self._template_context = template_params

    def clear_template_mapping(self):
        # type: () -> None
        """Clear the current template mapping parameters for string.Template formatting."""
        assert self._template_context
        self._template_context = None

    def write_template(self, template):
        # type: (unicode) -> None
        """Write a template to the stream."""
        msg = common.template_format(template, self._template_context)
        self._stream.write(_indent_text(self._indent, msg))
        self._stream.write("\n")

    def write_empty_line(self):
        # type: () -> None
        """Write a line to the stream."""
        self._stream.write("\n")


class TemplateContext(object):
    """Set the template context for the writer."""

    def __init__(self, writer, template_params):
        # type: (IndentedTextWriter, Mapping[unicode,unicode]) -> None
        """Create a template context."""
        self._writer = writer
        self._template_context = template_params

    def __enter__(self):
        # type: () -> None
        """Set the template mapping for the writer."""
        self._writer.set_template_mapping(self._template_context)

    def __exit__(self, *args):
        # type: (*str) -> None
        """Clear the template mapping for the writer."""
        self._writer.clear_template_mapping()


class EmptyBlock(object):
    """Do not generate an indented block."""

    def __init__(self):
        # type: () -> None
        """Create an empty block."""
        pass

    def __enter__(self):
        # type: () -> None
        """Do nothing."""
        pass

    def __exit__(self, *args):
        # type: (*str) -> None
        """Do nothing."""
        pass


class IndentedScopedBlock(object):
    """Generate a block, template the parameters, and indent the contents."""

    def __init__(self, writer, opening, closing):
        # type: (IndentedTextWriter, unicode, unicode) -> None
        """Create a block."""
        self._writer = writer
        self._opening = opening
        self._closing = closing

    def __enter__(self):
        # type: () -> None
        """Write the beginning of the block and then indent."""
        self._writer.write_template(self._opening)
        self._writer.indent()

    def __exit__(self, *args):
        # type: (*str) -> None
        """Unindent the block and print the ending."""
        self._writer.unindent()
        self._writer.write_template(self._closing)
