"""SCons.Tool.mslib

Tool-specific initialization for lib (MicroSoft library archiver).

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.

"""

#
# __COPYRIGHT__
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
#

__revision__ = "__FILE__ __REVISION__ __DATE__ __DEVELOPER__"

import os

import SCons.Defaults
import SCons.Tool
import SCons.Tool.msvs
import SCons.Tool.msvc
import SCons.Util

from SCons.Tool.MSCommon import (
    msvc_setup_env_tool,
    msvc_setup_env_once,
)

tool_name = 'mslib'

def generate(env) -> None:
    """Add Builders and construction variables for lib to an Environment."""
    SCons.Tool.createStaticLibBuilder(env)

    # Set-up ms tools paths
    msvc_setup_env_once(env, tool=tool_name)

    env['AR']          = 'lib'
    env['ARFLAGS']     = SCons.Util.CLVar('/nologo')
    env['ARCOM']       = "${TEMPFILE('$AR $ARFLAGS /OUT:$TARGET $SOURCES','$ARCOMSTR')}"
    env['LIBPREFIX']   = ''
    env['LIBSUFFIX']   = '.lib'

    # Issue #3350
    # Change tempfile argument joining character from a space to a newline
    # mslink will fail if any single line is too long, but is fine with many lines
    # in a tempfile
    env['TEMPFILEARGJOIN'] = os.linesep


def exists(env):
    return msvc_setup_env_tool(env, tool=tool_name)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
