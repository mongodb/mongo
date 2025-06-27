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

"""Tool-specific initialization for clang++.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.
"""

# Based on SCons/Tool/g++.py by Paweł Tomulik 2014 as a separate tool.
# Brought into the SCons mainline by Russel Winder 2017.

import os.path
import re
from subprocess import DEVNULL, PIPE

import SCons.Tool
import SCons.Util
import SCons.Tool.cxx
from SCons.Tool.clangCommon import get_clang_install_dirs
from SCons.Tool.MSCommon import msvc_setup_env_once


compilers = ['clang++']

def generate(env) -> None:
    """Add Builders and construction variables for clang++ to an Environment."""
    static_obj, shared_obj = SCons.Tool.createObjBuilders(env)

    SCons.Tool.cxx.generate(env)

    env['CXX'] = env.Detect(compilers) or 'clang++'

    # platform specific settings
    if env['PLATFORM'] == 'aix':
        env['SHCXXFLAGS'] = SCons.Util.CLVar('$CXXFLAGS -mminimal-toc')
        env['STATIC_AND_SHARED_OBJECTS_ARE_THE_SAME'] = 1
        env['SHOBJSUFFIX'] = '$OBJSUFFIX'
    elif env['PLATFORM'] == 'hpux':
        env['SHOBJSUFFIX'] = '.pic.o'
    elif env['PLATFORM'] == 'sunos':
        env['SHOBJSUFFIX'] = '.pic.o'
    elif env['PLATFORM'] == 'win32':
        # Ensure that we have a proper path for clang++
        clangxx = SCons.Tool.find_program_path(
            env, compilers[0], default_paths=get_clang_install_dirs(env['PLATFORM'])
        )
        if clangxx:
            clangxx_bin_dir = os.path.dirname(clangxx)
            env.AppendENVPath('PATH', clangxx_bin_dir)

            # Set-up ms tools paths
            msvc_setup_env_once(env)

    # determine compiler version
    if env['CXX']:
        kw = {
            'stdout': PIPE,
            'stderr': DEVNULL,
            'universal_newlines': True,
        }
        cp = SCons.Action.scons_subproc_run(env, [env['CXX'], '-dumpversion'], **kw)
        line = cp.stdout
        if line:
            env['CXXVERSION'] = line

    env['CCDEPFLAGS'] = '-MMD -MF ${TARGET}.d'
    env["NINJA_DEPFILE_PARSE_FORMAT"] = 'clang'


def exists(env):
    return env.Detect(compilers)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
