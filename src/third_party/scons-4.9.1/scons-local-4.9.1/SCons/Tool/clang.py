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

"""Tool-specific initialization for clang.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.
"""

# Based on SCons/Tool/gcc.py by Paweł Tomulik 2014 as a separate tool.
# Brought into the SCons mainline by Russel Winder 2017.

import os
import re
from subprocess import DEVNULL, PIPE

import SCons.Util
import SCons.Tool.cc
from SCons.Tool.clangCommon import get_clang_install_dirs
from SCons.Tool.MSCommon import msvc_setup_env_once


compilers = ['clang']


def generate(env) -> None:
    """Add Builders and construction variables for clang to an Environment."""
    SCons.Tool.cc.generate(env)

    if env['PLATFORM'] == 'win32':
        # Ensure that we have a proper path for clang
        clang = SCons.Tool.find_program_path(
            env, compilers[0], default_paths=get_clang_install_dirs(env['PLATFORM'])
        )
        if clang:
            clang_bin_dir = os.path.dirname(clang)
            env.AppendENVPath("PATH", clang_bin_dir)

            # Set-up ms tools paths
            msvc_setup_env_once(env)

    env['CC'] = env.Detect(compilers) or 'clang'
    if env['PLATFORM'] in ['cygwin', 'win32']:
        env['SHCCFLAGS'] = SCons.Util.CLVar('$CCFLAGS')
    else:
        env['SHCCFLAGS'] = SCons.Util.CLVar('$CCFLAGS -fPIC')

    # determine compiler version
    if env['CC']:
        kw = {
            'stdout': PIPE,
            'stderr': DEVNULL,
            'universal_newlines': True,
        }
        cp = SCons.Action.scons_subproc_run(env, [env['CC'], '-dumpversion'], **kw)
        line = cp.stdout
        if line:
            env['CCVERSION'] = line

    env['CCDEPFLAGS'] = '-MMD -MF ${TARGET}.d'
    env["NINJA_DEPFILE_PARSE_FORMAT"] = 'clang'

def exists(env):
    return env.Detect(compilers)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
