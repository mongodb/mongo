# Copyright 2021 MongoDB Inc.
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

import SCons

def _add_scanner(builder):
    # We are taking over the target scanner here. If we want to not do
    # that we need to invent a ListScanner concept to inject. What if
    # the other scanner wants a different path_function?
    assert builder.target_scanner is None

    def new_scanner(node, env, path, argument):
        # Use the path information that FindPathDirs gave us to resolve
        # the forced includes into nodes given the search path.
        fis = [env.FindFile(f, path) for f in env.get('FORCEINCLUDES', [])]

        # Use the nodes *source* scanner, which was provided to us as
        # `argument` when we created this scanner, to scan the forced
        # includes for transitive includes.
        node.get_executor().scan(scanner=argument, node_list=fis)

        # The forced includes will be added as implicit dependencies
        # for us when we return them.
        return fis

    # The 'builder.builder' here is because we need to reach inside
    # the CompositeBuilder that wraps the object builders that come
    # back from createObjBuilders.
    builder.builder.target_scanner = SCons.Scanner.Scanner(
        function=new_scanner,
        path_function=SCons.Script.FindPathDirs('CPPPATH'),
        argument=builder.source_scanner,
    )

def generate(env, **kwargs):
    if not 'FORCEINCLUDEPREFIX' in env:
        if 'msvc' in env.get('TOOLS', []):
            env['FORCEINCLUDEPREFIX'] = '/FI'
        else:
            env['FORCEINCLUDEPREFIX'] = '-include '

    if not 'FORCEINCLUDESUFFIX' in env:
        env['FORCEINCLUDESUFFIX'] = ''

    # Expand FORCEINCLUDES with the indicated prefixes and suffixes.
    env['_FORCEINCLUDES'] = '${_concat(FORCEINCLUDEPREFIX, FORCEINCLUDES, FORCEINCLUDESUFFIX, __env__, lambda x: x, TARGET, SOURCE)}'

    env.Append(
        # It might be better if this went in _CPPINCFLAGS, but it
        # breaks the MSVC RC builder because the `rc` tool doesn't
        # honor /FI.  It should be OK to put it in CCFLAGS, unless
        # there is a compiler that requires that an forced include
        # only come after the include file search path arguments that
        # would enable discovery.
        CCFLAGS=[
            '$_FORCEINCLUDES',
        ]
    )

    for object_builder in SCons.Tool.createObjBuilders(env):
        _add_scanner(object_builder)

def exists(env):
    return True
