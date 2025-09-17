# MIT License
#
# Copyright The SCons Foundation
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""Variable type for path Variables.

To be used whenever a user-specified path override setting should be allowed.

Arguments to PathVariable are:
  * *key* - name of this variable on the command line (e.g. "prefix")
  * *help* - help string for variable
  * *default* - default value for this variable
  * *validator* - [optional] validator for variable value.  Predefined are:

    * *PathAccept* - accepts any path setting; no validation
    * *PathIsDir* - path must be an existing directory
    * *PathIsDirCreate* - path must be a dir; will create
    * *PathIsFile* - path must be a file
    * *PathExists* - path must exist (any type) [default]

  The *validator* is a function that is called and which should return
  True or False to indicate if the path is valid.  The arguments
  to the validator function are: (*key*, *val*, *env*).  *key* is the
  name of the variable, *val* is the path specified for the variable,
  and *env* is the environment to which the Variables have been added.

Usage example::

    opts = Variables()
    opts.Add(
        PathVariable(
            'qtdir',
            help='where the root of Qt is installed',
            default=qtdir,
            validator=PathIsDir,
        )
    )
    opts.Add(
        PathVariable(
            'qt_includes',
            help='where the Qt includes are installed',
            default='$qtdir/includes',
            validator=PathIsDirCreate,
        )
    )
    opts.Add(
        PathVariable(
            'qt_libraries',
            help='where the Qt library is installed',
            default='$qtdir/lib',
        )
    )
"""

from __future__ import annotations

import os
import os.path
from typing import Callable

import SCons.Errors
import SCons.Util

__all__ = ['PathVariable',]

class _PathVariableClass:
    """Class implementing path variables.

    This class exists mainly to expose the validators without code having
    to import the names: they will appear as methods of ``PathVariable``,
    a statically created instance of this class, which is placed in
    the SConscript namespace.

    Instances are callable to produce a suitable variable tuple.
    """

    @staticmethod
    def PathAccept(key: str, val, env) -> None:
        """Validate path with no checking."""
        return

    @staticmethod
    def PathIsDir(key: str, val, env) -> None:
        """Validate path is a directory."""
        if os.path.isdir(val):
            return
        if os.path.isfile(val):
            msg = f'Directory path for variable {key!r} is a file: {val}'
        else:
            msg = f'Directory path for variable {key!r} does not exist: {val}'
        raise SCons.Errors.UserError(msg)

    @staticmethod
    def PathIsDirCreate(key: str, val, env) -> None:
        """Validate path is a directory, creating if needed."""
        if os.path.isdir(val):
            return
        try:
            os.makedirs(val, exist_ok=True)
        except FileExistsError as exc:
            msg = f'Path for variable {key!r} is a file, not a directory: {val}'
            raise SCons.Errors.UserError(msg) from exc
        except (PermissionError, OSError) as exc:
            msg = f'Path for variable {key!r} could not be created: {val}'
            raise SCons.Errors.UserError(msg) from exc

    @staticmethod
    def PathIsFile(key: str, val, env) -> None:
        """Validate path is a file."""
        if not os.path.isfile(val):
            if os.path.isdir(val):
                msg = f'File path for variable {key!r} is a directory: {val}'
            else:
                msg = f'File path for variable {key!r} does not exist: {val}'
            raise SCons.Errors.UserError(msg)

    @staticmethod
    def PathExists(key: str, val, env) -> None:
        """Validate path exists."""
        if not os.path.exists(val):
            msg = f'Path for variable {key!r} does not exist: {val}'
            raise SCons.Errors.UserError(msg)

    # lint: W0622: Redefining built-in 'help' (redefined-builtin)
    def __call__(
        self, key: str, help: str, default, validator: Callable | None = None
    ) -> tuple[str, str, str, Callable, None]:
        """Return a tuple describing a path list SCons Variable.

        The input parameters describe a 'path list' variable. Returns
        a tuple with the correct converter and validator appended. The
        result is usable for input to :meth:`Add`.

        The *default* parameter specifies the default path to use if the
        user does not specify an override with this variable.

        *validator* is a validator, see this file for examples
        """
        if validator is None:
            validator = self.PathExists

        if SCons.Util.is_List(key) or SCons.Util.is_Tuple(key):
            helpmsg = f'{help} ( /path/to/{key[0]} )'
        else:
            helpmsg = f'{help} ( /path/to/{key} )'
        return key, helpmsg, default, validator, None


PathVariable = _PathVariableClass()

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
