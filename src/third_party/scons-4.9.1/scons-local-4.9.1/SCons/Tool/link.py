#
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

"""
Tool-specific initialization for the generic POSIX linker.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.

"""

import SCons.Tool
import SCons.Util
import SCons.Warnings
from SCons.Tool import createProgBuilder
from SCons.Tool.linkCommon import smart_link
from SCons.Tool.linkCommon.LoadableModule import setup_loadable_module_logic
from SCons.Tool.linkCommon.SharedLibrary import setup_shared_lib_logic


def generate(env) -> None:
    """Add Builders and construction variables for gnulink to an Environment."""
    createProgBuilder(env)

    setup_shared_lib_logic(env)
    setup_loadable_module_logic(env)

    env['SMARTLINK'] = smart_link
    env['LINK'] = "$SMARTLINK"
    env['LINKFLAGS'] = SCons.Util.CLVar('')

    # __RPATH is only set to something ($_RPATH typically) on platforms that support it.
    env['LINKCOM'] = '$LINK -o $TARGET $LINKFLAGS $__RPATH $SOURCES $_LIBDIRFLAGS $_LIBFLAGS'
    env['LIBDIRPREFIX'] = '-L'
    env['LIBDIRSUFFIX'] = ''
    env['_LIBFLAGS'] = '${_stripixes(LIBLINKPREFIX, LIBS, LIBLINKSUFFIX, LIBPREFIXES, LIBSUFFIXES, __env__, LIBLITERALPREFIX)}'
    env['LIBLINKPREFIX'] = '-l'
    env['LIBLINKSUFFIX'] = ''


def exists(env):
    # This module isn't really a Tool on its own, it's common logic for
    # other linkers.
    return None

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
