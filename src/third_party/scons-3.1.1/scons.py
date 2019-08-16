#! /usr/bin/env python
#
# SCons - a Software Constructor
#
# Copyright (c) 2001 - 2019 The SCons Foundation
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

from __future__ import print_function

__revision__ = "src/script/scons.py 72ae09dc35ac2626f8ff711d8c4b30b6138e08e3 2019-08-08 14:50:06 bdeegan"

__version__ = "3.1.1"

__build__ = "72ae09dc35ac2626f8ff711d8c4b30b6138e08e3"

__buildsys__ = "octodog"

__date__ = "2019-08-08 14:50:06"

__developer__ = "bdeegan"

# This is the entry point to the SCons program.
# The only job of this script is to work out where the guts of the program
# could be and import them, where the real work begins.
# SCons can be invoked several different ways
# - from an installed location
# - from a "local install" copy
# - from a source tree, which has a different dir struture than the other two
# Try to account for all those possibilities.

import os
import sys

##############################################################################
# BEGIN STANDARD SCons SCRIPT HEADER
#
# This is the cut-and-paste logic so that a self-contained script can
# interoperate correctly with different SCons versions and installation
# locations for the engine.  If you modify anything in this section, you
# should also change other scripts that use this same header.
##############################################################################

# compatibility check
if (3,0,0) < sys.version_info < (3,5,0) or sys.version_info < (2,7,0):
    msg = "scons: *** SCons version %s does not run under Python version %s.\n\
Python 2.7 or >= 3.5 is required.\n"
    sys.stderr.write(msg % (__version__, sys.version.split()[0]))
    sys.exit(1)

# Strip the script directory from sys.path so on case-insensitive
# (WIN32) systems Python doesn't think that the "scons" script is the
# "SCons" package.
script_dir = os.path.dirname(os.path.realpath(__file__))
script_path = os.path.realpath(os.path.dirname(__file__))
if script_path in sys.path:
    sys.path.remove(script_path)

libs = []

if "SCONS_LIB_DIR" in os.environ:
    libs.append(os.environ["SCONS_LIB_DIR"])

# running from source takes 2nd priority (since 2.3.2), following SCONS_LIB_DIR
source_path = os.path.join(script_path, os.pardir, 'engine')
if os.path.isdir(source_path):
    libs.append(source_path)

# add local-install locations
local_version = 'scons-local-' + __version__
local = 'scons-local'
if script_dir:
    local_version = os.path.join(script_dir, local_version)
    local = os.path.join(script_dir, local)
if os.path.isdir(local_version):
    libs.append(os.path.abspath(local_version))
if os.path.isdir(local):
    libs.append(os.path.abspath(local))

scons_version = 'scons-%s' % __version__

# preferred order of scons lookup paths
prefs = []

# if we can find package information, use it
try:
    import pkg_resources
except ImportError:
    pass
else:
    try:
        d = pkg_resources.get_distribution('scons')
    except pkg_resources.DistributionNotFound:
        pass
    else:
        prefs.append(d.location)

if sys.platform == 'win32':
    # Use only sys.prefix on Windows
    prefs.append(sys.prefix)
    prefs.append(os.path.join(sys.prefix, 'Lib', 'site-packages'))
else:
    # On other (POSIX) platforms, things are more complicated due to
    # the variety of path names and library locations.
    # Build up some possibilities, then transform them into candidates
    temp = []
    if script_dir == 'bin':
        # script_dir is `pwd`/bin;
        # check `pwd`/lib/scons*.
        temp.append(os.getcwd())
    else:
        if script_dir == '.' or script_dir == '':
            script_dir = os.getcwd()
        head, tail = os.path.split(script_dir)
        if tail == "bin":
            # script_dir is /foo/bin;
            # check /foo/lib/scons*.
            temp.append(head)

    head, tail = os.path.split(sys.prefix)
    if tail == "usr":
        # sys.prefix is /foo/usr;
        # check /foo/usr/lib/scons* first,
        # then /foo/usr/local/lib/scons*.
        temp.append(sys.prefix)
        temp.append(os.path.join(sys.prefix, "local"))
    elif tail == "local":
        h, t = os.path.split(head)
        if t == "usr":
            # sys.prefix is /foo/usr/local;
            # check /foo/usr/local/lib/scons* first,
            # then /foo/usr/lib/scons*.
            temp.append(sys.prefix)
            temp.append(head)
        else:
            # sys.prefix is /foo/local;
            # check only /foo/local/lib/scons*.
            temp.append(sys.prefix)
    else:
        # sys.prefix is /foo (ends in neither /usr or /local);
        # check only /foo/lib/scons*.
        temp.append(sys.prefix)

    # suffix these to add to our original prefs:
    prefs.extend([os.path.join(x, 'lib') for x in temp])
    prefs.extend([os.path.join(x, 'lib', 'python' + sys.version[:3],
                               'site-packages') for x in temp])


    # Add the parent directory of the current python's library to the
    # preferences.  This picks up differences between, e.g., lib and lib64,
    # and finds the base location in case of a non-copying virtualenv.
    try:
        libpath = os.__file__
    except AttributeError:
        pass
    else:
        # Split /usr/libfoo/python*/os.py to /usr/libfoo/python*.
        libpath, tail = os.path.split(libpath)
        # Split /usr/libfoo/python* to /usr/libfoo
        libpath, tail = os.path.split(libpath)
        # Check /usr/libfoo/scons*.
        prefs.append(libpath)

# Look first for 'scons-__version__' in all of our preference libs,
# then for 'scons'.  Skip paths that do not exist.
libs.extend([os.path.join(x, scons_version) for x in prefs if os.path.isdir(x)])
libs.extend([os.path.join(x, 'scons') for x in prefs if os.path.isdir(x)])

sys.path = libs + sys.path

##############################################################################
# END STANDARD SCons SCRIPT HEADER
##############################################################################

if __name__ == "__main__":
    try:
        import SCons.Script
    except ImportError:
        sys.stderr.write("SCons import failed. Unable to find engine files in:\n")
        for path in libs:
            sys.stderr.write("  {}\n".format(path))
        raise

    # this does all the work, and calls sys.exit
    # with the proper exit status when done.
    SCons.Script.main()

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
