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

"""SCons exception classes.

Used to handle internal and user errors in SCons.
"""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

from SCons.Util.sctypes import to_String, is_String

if TYPE_CHECKING:
    from SCons.Executor import Executor

# Note that not all Errors are defined here, some are at the point of use


class BuildError(Exception):
    """SCons Errors that can occur while building.

    A :class:`BuildError` exception contains information both
    about the erorr itself, and what caused the error.

    Attributes:
       node: (*cause*) the error occurred while building this target node(s)
       errstr: (*info*) a description of the error message
       status: (*info*) the return code of the action that caused the build error.
          Must be set to a non-zero value even if the build error is not due
          to an action returning a non-zero returned code.
       exitstatus: (*info*) SCons exit status due to this build error.
          Must be nonzero unless due to an explicit :meth:`Exit` call.
          Not always the same as ``status``, since actions return a status
          code that should be respected, but SCons typically exits with 2
          irrespective of the return value of the failed action.
       filename: (*info*) The name of the file or directory that caused the
          build error. Set to ``None`` if no files are associated with
          this error. This might be different from the target
          being built. For example, failure to create the
          directory in which the target file will appear. It
          can be ``None`` if the error is not due to a particular
          filename.
       executor: (*cause*) the executor that caused the build to fail (might
          be ``None`` if the build failures is not due to the executor failing)
       action: (*cause*) the action that caused the build to fail (might be
          ``None`` if the build failures is not due to the an
          action failure)
       command: (*cause*) the command line for the action that caused the
          build to fail (might be ``None`` if the build failures
          is not due to the an action failure)
       exc_info: (*info*) Info about exception that caused the build
          error. Set to ``(None, None, None)`` if this build
          error is not due to an exception.

    """

    def __init__(self,
                 node=None, errstr: str="Unknown error", status: int=2, exitstatus: int=2,
                 filename=None, executor: Executor | None = None, action=None, command=None,
                 exc_info=(None, None, None)) -> None:

        # py3: errstr should be string and not bytes.

        self.errstr = to_String(errstr)
        self.status = status
        self.exitstatus = exitstatus
        self.filename = filename
        self.exc_info = exc_info

        self.node = node
        self.executor = executor
        self.action = action
        self.command = command

        super().__init__(node, errstr, status, exitstatus, filename,
                         executor, action, command, exc_info)

    def __str__(self) -> str:
        if self.filename:
            return self.filename + ': ' + self.errstr
        else:
            return self.errstr

class InternalError(Exception):
    pass

class UserError(Exception):
    pass

class StopError(Exception):
    pass

class SConsEnvironmentError(Exception):
    pass

class MSVCError(IOError):
    pass

class ExplicitExit(Exception):
    def __init__(self, node=None, status=None, *args) -> None:
        self.node = node
        self.status = status
        self.exitstatus = status
        super().__init__(*args)

def convert_to_BuildError(status, exc_info=None):
    """Convert a return code to a BuildError Exception.

    The `buildError.status` we set here will normally be
    used as the exit status of the "scons" process.

    Args:
      status: can either be a return code or an Exception.
      exc_info (tuple, optional): explicit exception information.

    """

    if not exc_info and isinstance(status, Exception):
        exc_info = (status.__class__, status, None)


    if isinstance(status, BuildError):
        buildError = status
        buildError.exitstatus = 2   # always exit with 2 on build errors
    elif isinstance(status, ExplicitExit):
        status = status.status
        errstr = 'Explicit exit, status %s' % status
        buildError = BuildError(
            errstr=errstr,
            status=status,      # might be 0, OK here
            exitstatus=status,      # might be 0, OK here
            exc_info=exc_info)
    elif isinstance(status, (StopError, UserError)):
        buildError = BuildError(
            errstr=str(status),
            status=2,
            exitstatus=2,
            exc_info=exc_info)
    elif isinstance(status, shutil.SameFileError):
        # PY3 has a exception for when copying file to itself
        # It's object provides info differently than below
        try:
            filename = status.filename
        except AttributeError:
            filename = None

        buildError = BuildError(
            errstr=status.args[0],
            status=status.errno,
            exitstatus=2,
            filename=filename,
            exc_info=exc_info)

    elif isinstance(status, (SConsEnvironmentError, OSError, IOError)):
        # If an IOError/OSError happens, raise a BuildError.
        # Report the name of the file or directory that caused the
        # error, which might be different from the target being built
        # (for example, failure to create the directory in which the
        # target file will appear).
        filename = getattr(status, 'filename', None)
        strerror = getattr(status, 'strerror', None)
        if strerror is None:
            strerror = str(status)
        errno = getattr(status, 'errno', None)
        if errno is None:
            errno = 2

        buildError = BuildError(
            errstr=strerror,
            status=errno,
            exitstatus=2,
            filename=filename,
            exc_info=exc_info)
    elif isinstance(status, Exception):
        buildError = BuildError(
            errstr='%s : %s' % (status.__class__.__name__, status),
            status=2,
            exitstatus=2,
            exc_info=exc_info)
    elif is_String(status):
        buildError = BuildError(
            errstr=status,
            status=2,
            exitstatus=2)
    else:
        buildError = BuildError(
            errstr="Error %s" % status,
            status=status,
            exitstatus=2)

    #import sys
    #sys.stderr.write("convert_to_BuildError: status %s => (errstr %s, status %s)\n"%(status,buildError.errstr, buildError.status))
    return buildError

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
