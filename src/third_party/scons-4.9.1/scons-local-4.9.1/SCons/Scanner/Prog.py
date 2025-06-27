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

"""Dependency scanner for program files."""

import SCons.Node
import SCons.Node.FS
import SCons.Util
from . import ScannerBase, FindPathDirs

# global, set by --debug=findlibs
print_find_libs = None

def ProgramScanner(**kwargs):
    """Return a prototype Scanner instance for scanning executable
    files for static-lib dependencies"""
    kwargs['path_function'] = FindPathDirs('LIBPATH')
    ps = ScannerBase(scan, "ProgramScanner", **kwargs)
    return ps

def _subst_libs(env, libs):
    """Substitute environment variables and split into list."""
    if SCons.Util.is_String(libs):
        libs = env.subst(libs)
        if SCons.Util.is_String(libs):
            libs = libs.split()
    elif SCons.Util.is_Sequence(libs):
        _libs = []
        for l in libs:
            _libs += _subst_libs(env, l)
        libs = _libs
    else:
        # libs is an object (Node, for example)
        libs = [libs]
    return libs

def scan(node, env, libpath = ()):
    """Scans program files for static-library dependencies.

    It will search the LIBPATH environment variable
    for libraries specified in the LIBS variable, returning any
    files it finds as dependencies.
    """
    try:
        libs = env['LIBS']
    except KeyError:
        # There are no LIBS in this environment, so just return a null list:
        return []

    libs = _subst_libs(env, libs)

    try:
        prefix = env['LIBPREFIXES']
        if not SCons.Util.is_List(prefix):
            prefix = [ prefix ]
    except KeyError:
        prefix = [ '' ]

    try:
        suffix = env['LIBSUFFIXES']
        if not SCons.Util.is_List(suffix):
            suffix = [ suffix ]
    except KeyError:
        suffix = [ '' ]

    pairs = []
    for suf in map(env.subst, suffix):
        for pref in map(env.subst, prefix):
            pairs.append((pref, suf))

    result = []

    if callable(libpath):
        libpath = libpath()

    find_file = SCons.Node.FS.find_file
    adjustixes = SCons.Util.adjustixes
    for lib in libs:
        if SCons.Util.is_String(lib):
            for pref, suf in pairs:
                l = adjustixes(lib, pref, suf)
                l = find_file(l, libpath, verbose=print_find_libs)
                if l:
                    result.append(l)
        else:
            result.append(lib)

    return result

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
