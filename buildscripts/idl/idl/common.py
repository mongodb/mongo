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
IDL Common classes.

Classes which are shared among both the IDL idl.syntax and idl.AST trees.
"""

from __future__ import absolute_import, print_function, unicode_literals

import os
import string
from typing import Mapping

COMMAND_NAMESPACE_CONCATENATE_WITH_DB = "concatenate_with_db"
COMMAND_NAMESPACE_CONCATENATE_WITH_DB_OR_UUID = "concatenate_with_db_or_uuid"
COMMAND_NAMESPACE_IGNORED = "ignored"
COMMAND_NAMESPACE_TYPE = "type"


def title_case(name):
    # type: (unicode) -> unicode
    """Return a CapitalCased version of a string."""

    # Only capitalize the last part of a fully-qualified name
    pos = name.rfind("::")
    if pos > -1:
        return name[:pos + 2] + name[pos + 2:pos + 3].upper() + name[pos + 3:]

    return name[0:1].upper() + name[1:]


def camel_case(name):
    # type: (unicode) -> unicode
    """Return a camelCased version of a string."""
    return name[0:1].lower() + name[1:]


def qualify_cpp_name(cpp_namespace, cpp_type_name):
    # type: (unicode, unicode) -> unicode
    """Preprend a type name with a C++ namespace if cpp_namespace is not None."""
    if cpp_namespace:
        return cpp_namespace + "::" + cpp_type_name

    return cpp_type_name


def _escape_template_string(template):
    # type: (unicode) -> unicode
    """Escape the '$' in template strings unless followed by '{'."""
    # See https://docs.python.org/2/library/string.html#template-strings
    template = template.replace('${', '#{')
    template = template.replace('$', '$$')
    return template.replace('#{', '${')


def template_format(template, template_params=None):
    # type: (unicode, Mapping[unicode,unicode]) -> unicode
    """Write a template to the stream."""
    # Ignore the types since we use unicode literals and this expects str but works fine with
    # unicode.
    # See https://docs.python.org/2/library/string.html#template-strings
    template = _escape_template_string(template)
    return string.Template(template).substitute(template_params)  # type: ignore


def template_args(template, **kwargs):
    # type: (unicode, **unicode) -> unicode
    """Write a template to the stream."""
    # Ignore the types since we use unicode literals and this expects str but works fine with
    # unicode.
    # See https://docs.python.org/2/library/string.html#template-strings
    template = _escape_template_string(template)
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
