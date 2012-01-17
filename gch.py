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

# 1.1
#
# 09-11-2011 Pedro Larroy: Fixed dependency emitter not working with variant dir
#
#


import SCons.Action
import SCons.Builder
import SCons.Scanner.C
import SCons.Util
import SCons.Script
import os

SCons.Script.EnsureSConsVersion(0,96,92)

GchAction = SCons.Action.Action('$GCHCOM', '$GCHCOMSTR')
GchShAction = SCons.Action.Action('$GCHSHCOM', '$GCHSHCOMSTR')

def gen_suffix(env, sources):
    return sources[0].get_suffix() + env['GCHSUFFIX']

GchShBuilder = SCons.Builder.Builder(action = GchShAction,
                                     source_scanner = SCons.Scanner.C.CScanner(),
                                     suffix = gen_suffix)

GchBuilder = SCons.Builder.Builder(action = GchAction,
                                   source_scanner = SCons.Scanner.C.CScanner(),
                                   suffix = gen_suffix)

def header_path(node):
    h_path = node.abspath
    idx = h_path.rfind('.gch')
    if idx != -1:
        h_path = h_path[0:idx]
        if not os.path.isfile(h_path):
            raise SCons.Errors.StopError("can't find header file: {0}".format(h_path))
        return h_path

    else:
        raise SCons.Errors.StopError("{0} file doesn't have .gch extension".format(h_path))


def static_pch_emitter(target,source,env):
    SCons.Defaults.StaticObjectEmitter( target, source, env )
    scanner = SCons.Scanner.C.CScanner()
    path = scanner.path(env)

    deps = scanner(source[0], env, path)
    if env.get('Gch'):
        h_path = header_path(env['Gch'])
        if h_path in [x.abspath for x in deps]:
            #print 'Found dep. on pch: ', target[0], ' -> ', env['Gch']
            env.Depends(target, env['Gch'])

    return (target, source)

def shared_pch_emitter(target,source,env):
    SCons.Defaults.SharedObjectEmitter( target, source, env )

    scanner = SCons.Scanner.C.CScanner()
    path = scanner.path(env)
    deps = scanner(source[0], env, path)

    if env.get('GchSh'):
        h_path = header_path(env['GchSh'])
        if h_path in [x.abspath for x in deps]:
            #print 'Found dep. on pch (shared): ', target[0], ' -> ', env['Gch']
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

    env['GCHCOM']     = '$CXX -Wall -o $TARGET -x c++-header -c $CXXFLAGS $CCFLAGS $_CCCOMCOM $SOURCE'
    env['GCHSHCOM']   = '$CXX -o $TARGET -x c++-header -c $SHCXXFLAGS $CCFLAGS $_CCCOMCOM $SOURCE'
    env['GCHSUFFIX']  = '.gch'

    for suffix in SCons.Util.Split('.c .C .cc .cxx .cpp .c++'):
        env['BUILDERS']['StaticObject'].add_emitter( suffix, static_pch_emitter )
        env['BUILDERS']['SharedObject'].add_emitter( suffix, shared_pch_emitter )


def exists(env):
    return env.Detect('g++')
