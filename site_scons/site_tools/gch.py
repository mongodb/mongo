# $Id$
#
# SCons builder for gcc's precompiled headers
# Copyright (C) 2006  Tim Blechmann
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

# $Revision$
# $LastChangedRevision$
# $LastChangedDate$
# $LastChangedBy$

import SCons.Action
import SCons.Builder
import SCons.Scanner.C
import SCons.Util
import SCons.Script

SCons.Script.EnsureSConsVersion(0,96,92)

GchAction = SCons.Action.Action('$GCHCOM', '$GCHCOMSTR')
GchShAction = SCons.Action.Action('$GCHSHCOM', '$GCHSHCOMSTR')

def gen_suffix(env, sources):
    return sources[0].get_suffix() + env['GCHSUFFIX']

def header_path(env, node):
    assert len(node.sources) == 1
    path = node.sources[0].path
    return path

GchShBuilder = SCons.Builder.Builder(action = GchShAction,
                                     source_scanner = SCons.Scanner.C.CScanner(),
                                     suffix = gen_suffix)

GchBuilder = SCons.Builder.Builder(action = GchAction,
                                   source_scanner = SCons.Scanner.C.CScanner(),
                                   suffix = gen_suffix)

def static_pch_emitter(target,source,env):
    SCons.Defaults.StaticObjectEmitter( target, source, env )

    scanner = SCons.Scanner.C.CScanner()
    path = scanner.path(env)
    deps = scanner(source[0], env, path)

    if env.has_key('Gch') and env['Gch']:
        if header_path(env, env['Gch']) in [x.path for x in deps]:
            env.Depends(target, env['Gch'])

    return (target, source)

def shared_pch_emitter(target,source,env):
    SCons.Defaults.SharedObjectEmitter( target, source, env )

    scanner = SCons.Scanner.C.CScanner()
    path = scanner.path(env)
    deps = scanner(source[0], env, path)

    if env.has_key('GchSh') and env['GchSh']:
        if header_path(env, env['GchSh']) in [x.path for x in deps]:
            env.Depends(target, env['GchSh'])
    return (target, source)

def generate(env):
    """
    Add builders and construction variables for the Gch builder.
    """
    env.Append(BUILDERS = {
        'gch': env.Builder(
        action = GchAction,
        target_factory = env.fs.File,
        ),
        'gchsh': env.Builder(
        action = GchShAction,
        target_factory = env.fs.File,
        ),
        })

    try:
        bld = env['BUILDERS']['Gch']
        bldsh = env['BUILDERS']['GchSh']
    except KeyError:
        bld = GchBuilder
        bldsh = GchShBuilder
        env['BUILDERS']['Gch'] = bld
        env['BUILDERS']['GchSh'] = bldsh

    env['GCHCOM']     = '$CXX -o $TARGET -x c++-header -c $CXXFLAGS $_CCCOMCOM $SOURCE'
    env['GCHSHCOM']   = '$CXX -o $TARGET -x c++-header -c $SHCXXFLAGS $_CCCOMCOM $SOURCE'
    env['GCHSUFFIX']  = '.gch'

    for suffix in SCons.Util.Split('.c .C .cc .cxx .cpp .c++'):
        env['BUILDERS']['StaticObject'].add_emitter( suffix, static_pch_emitter )
        env['BUILDERS']['SharedObject'].add_emitter( suffix, shared_pch_emitter )


def exists(env):
    return env.Detect('g++')
