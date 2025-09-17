"""SCons.Tool.ipkg

Tool-specific initialization for ipkg.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.

The ipkg tool calls the ipkg-build. Its only argument should be the 
packages fake_root.
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

import SCons.Builder

def generate(env) -> None:
    """Add Builders and construction variables for ipkg to an Environment."""
    try:
        bld = env['BUILDERS']['Ipkg']
    except KeyError:
        bld = SCons.Builder.Builder(action='$IPKGCOM',
                                    suffix='$IPKGSUFFIX',
                                    source_scanner=None,
                                    target_scanner=None)
        env['BUILDERS']['Ipkg'] = bld


    env['IPKG'] = 'ipkg-build'
    env['IPKGCOM'] = '$IPKG $IPKGFLAGS ${SOURCE}'

    if env.WhereIs('id'):
        with os.popen('id -un') as p:
            env['IPKGUSER'] = p.read().strip()
        with os.popen('id -gn') as p:
            env['IPKGGROUP'] = p.read().strip()
    env['IPKGFLAGS'] = SCons.Util.CLVar('-o $IPKGUSER -g $IPKGGROUP')
    env['IPKGSUFFIX'] = '.ipk'

def exists(env):
    """
    Can we find the tool
    """
    return env.Detect('ipkg-build')

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
