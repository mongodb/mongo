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
Tool-specific initialization for gfortran, the GNU Fortran compiler.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.
"""

from SCons.Util import CLVar
from SCons.Tool.FortranCommon import add_all_to_env, add_fortran_to_env

compilers = ['gfortran']


def generate(env) -> None:
    """Add Builders and construction variables for gfortran."""

    add_all_to_env(env)
    add_fortran_to_env(env)

    compiler = env.Detect(compilers) or 'gfortran'

    # fill in other dialects (FORTRAN dialect set by fortran.generate(),
    # but don't overwrite if they have been set manually.
    for dialect in ['FORTRAN', 'F77', 'F90', 'F95', 'F03', 'F08']:
        if dialect not in env:
            env[f'{dialect}'] = compiler
        if f'SH{dialect}' not in env:
            env[f'SH{dialect}'] = f'${dialect}'

        # The fortran module always sets the shlib FLAGS, but does not
        # include -fPIC, which is needed for the GNU tools. Rewrite if needed.
        if env['PLATFORM'] not in ['cygwin', 'win32']:
            env[f'SH{dialect}FLAGS'] = CLVar(f'${dialect}FLAGS -fPIC')
        env[f'INC{dialect}PREFIX'] = "-I"
        env[f'INC{dialect}SUFFIX'] = ""

    env['FORTRANMODDIRPREFIX'] = "-J"


def exists(env):
    return env.Detect('gfortran')

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
