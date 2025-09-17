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
Tool-specific initialization for newer versions of the Intel Fortran Compiler
for Linux/Windows (and possibly Mac OS X).

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.
"""

import SCons.Defaults
from SCons.Scanner.Fortran import FortranScan
from .FortranCommon import add_all_to_env


def generate(env) -> None:
    """Add Builders and construction variables for ifort to an Environment."""
    # ifort supports Fortran 90 and Fortran 95
    # Additionally, ifort recognizes more file extensions.
    fscan = FortranScan("FORTRANPATH")
    SCons.Tool.SourceFileScanner.add_scanner('.i', fscan)
    SCons.Tool.SourceFileScanner.add_scanner('.i90', fscan)

    if 'FORTRANFILESUFFIXES' not in env:
        env['FORTRANFILESUFFIXES'] = ['.i']
    else:
        env['FORTRANFILESUFFIXES'].append('.i')

    if 'F90FILESUFFIXES' not in env:
        env['F90FILESUFFIXES'] = ['.i90']
    else:
        env['F90FILESUFFIXES'].append('.i90')

    add_all_to_env(env)

    fc = 'ifort'

    for dialect in ['F77', 'F90', 'FORTRAN', 'F95']:
        env[f'{dialect}'] = fc
        env[f'SH{dialect}'] = f'${dialect}'
        if env['PLATFORM'] == 'posix':
            env[f'SH{dialect}FLAGS'] = SCons.Util.CLVar(f'${dialect}FLAGS -fPIC')

    if env['PLATFORM'] == 'win32':
        # On Windows, the ifort compiler specifies the object on the
        # command line with -object:, not -o.  Massage the necessary
        # command-line construction variables.
        for dialect in ['F77', 'F90', 'FORTRAN', 'F95']:
            for var in [f'{dialect}COM', f'{dialect}PPCOM',
                        f'SH{dialect}COM', f'SH{dialect}PPCOM']:
                env[var] = env[var].replace('-o $TARGET', '-object:$TARGET')
        env['FORTRANMODDIRPREFIX'] = "/module:"
    else:
        env['FORTRANMODDIRPREFIX'] = "-module "


def exists(env):
    return env.Detect('ifort')

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
