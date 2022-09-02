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
"""Text Writing Utilites."""

import io
import string
from typing import List, Mapping, Union

from . import common

# Number of spaces to indent code
_INDENT_SPACE_COUNT = 4


def _fill_spaces(count):
    # type: (int) -> str
    """Fill a string full of spaces."""
    fill = ''
    for _ in range(count * _INDENT_SPACE_COUNT):
        fill += ' '

    return fill


def _indent_text(count, unindented_text):
    # type: (int, str) -> str
    """Indent each line of a multi-line string."""
    lines = unindented_text.splitlines()
    fill = _fill_spaces(count)
    return '\n'.join(fill + line for line in lines)


def is_function(name):
    # type: (str) -> bool
    """
    Return True if a serializer/deserializer is function.

    A function is prefixed with '::' so that the IDL generated code calls it as a function instead
    of as a class method.
    """
    return name.startswith("::")


def get_method_name(name):
    # type: (str) -> str
    """Get a method name from a fully qualified method name."""
    pos = name.rfind('::')
    if pos == -1:
        return name
    return name[pos + 2:]


def get_method_name_from_qualified_method_name(name):
    # type: (str) -> str
    """Get a method name from a fully qualified method name."""
    # TODO: in the future, we may want to support full-qualified calls to static methods
    # Strip the global prefix from enum functions
    if name.startswith("::"):
        name = name[2:]

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
        self._template_context = None  # type: Mapping[str, str]

    def write_unindented_line(self, msg):
        # type: (str) -> None
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
        # type: (str) -> None
        """Write a line to the stream, no template formattin applied."""
        self._stream.write(_indent_text(self._indent, msg))
        self._stream.write("\n")

    def set_template_mapping(self, template_params):
        # type: (Mapping[str,str]) -> None
        """Set the current template mapping parameters for string.Template formatting."""
        assert not self._template_context
        self._template_context = template_params

    def clear_template_mapping(self):
        # type: () -> None
        """Clear the current template mapping parameters for string.Template formatting."""
        assert self._template_context
        self._template_context = None

    def write_template(self, template):
        # type: (str) -> None
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
        # type: (IndentedTextWriter, Mapping[str,str]) -> None
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


class WriterBlock(object):
    """Interface for block types below."""

    def __enter__(self):
        # type: () -> None
        """Open a block."""
        pass

    def __exit__(self, *args):
        # type: (*str) -> None
        """Close the block."""
        pass


class EmptyBlock(WriterBlock):
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


class IndentedScopedBlock(WriterBlock):
    """Generate a block, template the parameters, and indent the contents."""

    def __init__(self, writer, opening, closing):
        # type: (IndentedTextWriter, str, str) -> None
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


class NamespaceScopeBlock(WriterBlock):
    """Generate an unindented blocks for a list of namespaces, and do not indent the contents."""

    def __init__(self, indented_writer, namespaces):
        # type: (IndentedTextWriter, List[str]) -> None
        """Create a block."""
        self._writer = indented_writer
        self._namespaces = namespaces

    def __enter__(self):
        # type: () -> None
        """Write the beginning of the block and do not indent."""
        for namespace in self._namespaces:
            self._writer.write_unindented_line('namespace %s {' % (namespace))

    def __exit__(self, *args):
        # type: (*str) -> None
        """Write the end of the block and do not change indentation."""
        self._namespaces.reverse()

        for namespace in self._namespaces:
            self._writer.write_unindented_line('}  // namespace %s' % (namespace))


class UnindentedBlock(WriterBlock):
    """Generate a block without indentation."""

    def __init__(self, writer, opening, closing):
        # type: (IndentedTextWriter, str, str) -> None
        """Create a block."""
        self._writer = writer
        self._opening = opening
        self._closing = closing

    def __enter__(self):
        # type: () -> None
        """Write the beginning of the block."""
        self._writer.write_unindented_line(self._opening)

    def __exit__(self, *args):
        # type: (*str) -> None
        """Write the ending of the block."""
        self._writer.write_unindented_line(self._closing)


class MultiBlock(WriterBlock):
    """Proxy container for a list of WriterBlocks."""

    def __init__(self, blocks):
        # type: (MultiBlock, List[WriterBlock]) -> None
        """Create a multi-block."""
        self._blocks = blocks

    def __enter__(self):
        # type: () -> None
        """Enter each block forwards."""
        for i in self._blocks:
            i.__enter__()

    def __exit__(self, *args):
        # type: (*str) -> None
        """And leave each block in reverse."""
        for i in reversed(self._blocks):
            i.__exit__(*args)
