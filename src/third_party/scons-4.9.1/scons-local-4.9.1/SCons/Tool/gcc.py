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

"""SCons.Tool.gcc

Tool-specific initialization for gcc.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.

"""

from . import cc
import re
from subprocess import PIPE

import SCons.Util

compilers = ['gcc', 'cc']


def generate(env) -> None:
    """Add Builders and construction variables for gcc to an Environment."""

    if 'CC' not in env:
        env['CC'] = env.Detect(compilers) or compilers[0]

    cc.generate(env)

    if env['PLATFORM'] in ['cygwin', 'win32']:
        env['SHCCFLAGS'] = SCons.Util.CLVar('$CCFLAGS')
    else:
        env['SHCCFLAGS'] = SCons.Util.CLVar('$CCFLAGS -fPIC')
    # determine compiler version
    version = detect_version(env, env['CC'])
    if version:
        env['CCVERSION'] = version

    env['CCDEPFLAGS'] = '-MMD -MF ${TARGET}.d'
    env["NINJA_DEPFILE_PARSE_FORMAT"] = 'gcc'



def exists(env):
    # is executable, and is a GNU compiler (or accepts '--version' at least)
    return detect_version(env, env.Detect(env.get('CC', compilers)))


def detect_version(env, cc):
    """Return the version of the GNU compiler, or None if it is not a GNU compiler."""
    version = None
    cc = env.subst(cc)
    if not cc:
        return version

    # -dumpversion was added in GCC 3.0.  As long as we're supporting
    # GCC versions older than that, we should use --version and a
    # regular expression.
    # pipe = SCons.Action.scons_subproc_run(env, SCons.Util.CLVar(cc) + ['-dumpversion'],
    cp = SCons.Action.scons_subproc_run(
        env, SCons.Util.CLVar(cc) + ['--version'], stdout=PIPE
    )
    if cp.returncode:
        return version

    # -dumpversion variant:
    # line = cp.stdout.strip()
    # --version variant:
    try:
        line = SCons.Util.to_str(cp.stdout.splitlines()[0])
    except IndexError:
        return version

    # -dumpversion variant:
    # version = line
    # --version variant:
    match = re.search(r'[0-9]+(\.[0-9]+)+', line)
    if match:
        version = match.group(0)

    return version

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
