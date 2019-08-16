"""SCons.Tool.gcc

Tool-specific initialization for MinGW (http://www.mingw.org/)

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.

"""

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
#

__revision__ = "src/engine/SCons/Tool/mingw.py 72ae09dc35ac2626f8ff711d8c4b30b6138e08e3 2019-08-08 14:50:06 bdeegan"

import os
import os.path
import glob

import SCons.Action
import SCons.Builder
import SCons.Defaults
import SCons.Tool
import SCons.Util

mingw_paths = [
    r'c:\MinGW\bin',
    r'C:\cygwin64\bin',
    r'C:\msys64',
    r'C:\cygwin\bin',
    r'C:\msys',
]


def shlib_generator(target, source, env, for_signature):
    cmd = SCons.Util.CLVar(['$SHLINK', '$SHLINKFLAGS'])

    dll = env.FindIxes(target, 'SHLIBPREFIX', 'SHLIBSUFFIX')
    if dll: cmd.extend(['-o', dll])

    cmd.extend(['$SOURCES', '$_LIBDIRFLAGS', '$_LIBFLAGS'])

    implib = env.FindIxes(target, 'LIBPREFIX', 'LIBSUFFIX')
    if implib: cmd.append('-Wl,--out-implib,' + implib.get_string(for_signature))

    def_target = env.FindIxes(target, 'WINDOWSDEFPREFIX', 'WINDOWSDEFSUFFIX')
    insert_def = env.subst("$WINDOWS_INSERT_DEF")
    if insert_def not in ['', '0', 0] and def_target: \
            cmd.append('-Wl,--output-def,' + def_target.get_string(for_signature))

    return [cmd]


def shlib_emitter(target, source, env):
    dll = env.FindIxes(target, 'SHLIBPREFIX', 'SHLIBSUFFIX')
    no_import_lib = env.get('no_import_lib', 0)

    if not dll:
        raise SCons.Errors.UserError(
            "A shared library should have exactly one target with the suffix: %s Target(s) are:%s" % \
            (env.subst("$SHLIBSUFFIX"), ",".join([str(t) for t in target])))

    if not no_import_lib and \
            not env.FindIxes(target, 'LIBPREFIX', 'LIBSUFFIX'):
        # Create list of target libraries as strings
        targetStrings = env.ReplaceIxes(dll,
                                        'SHLIBPREFIX', 'SHLIBSUFFIX',
                                        'LIBPREFIX', 'LIBSUFFIX')

        # Now add file nodes to target list
        target.append(env.fs.File(targetStrings))

    # Append a def file target if there isn't already a def file target
    # or a def file source or the user has explicitly asked for the target
    # to be emitted.
    def_source = env.FindIxes(source, 'WINDOWSDEFPREFIX', 'WINDOWSDEFSUFFIX')
    def_target = env.FindIxes(target, 'WINDOWSDEFPREFIX', 'WINDOWSDEFSUFFIX')
    skip_def_insert = env.subst("$WINDOWS_INSERT_DEF") in ['', '0', 0]
    if not def_source and not def_target and not skip_def_insert:
        # Create list of target libraries and def files as strings
        targetStrings = env.ReplaceIxes(dll,
                                        'SHLIBPREFIX', 'SHLIBSUFFIX',
                                        'WINDOWSDEFPREFIX', 'WINDOWSDEFSUFFIX')

        # Now add file nodes to target list
        target.append(env.fs.File(targetStrings))

    return (target, source)


shlib_action = SCons.Action.Action(shlib_generator, '$SHLINKCOMSTR', generator=1)
ldmodule_action = SCons.Action.Action(shlib_generator, '$LDMODULECOMSTR', generator=1)

res_action = SCons.Action.Action('$RCCOM', '$RCCOMSTR')

res_builder = SCons.Builder.Builder(action=res_action, suffix='.o',
                                    source_scanner=SCons.Tool.SourceFileScanner)
SCons.Tool.SourceFileScanner.add_scanner('.rc', SCons.Defaults.CScan)

# This is what we search for to find mingw:
# key_program = 'mingw32-gcc'
key_program = 'mingw32-make'


def find_version_specific_mingw_paths():
    r"""
    One example of default mingw install paths is:
    C:\mingw-w64\x86_64-6.3.0-posix-seh-rt_v5-rev2\mingw64\bin

    Use glob'ing to find such and add to mingw_paths
    """
    new_paths = glob.glob(r"C:\mingw-w64\*\mingw64\bin")

    return new_paths


def generate(env):
    global mingw_paths
    # Check for reasoanble mingw default paths
    mingw_paths += find_version_specific_mingw_paths()

    mingw = SCons.Tool.find_program_path(env, key_program, default_paths=mingw_paths)
    if mingw:
        mingw_bin_dir = os.path.dirname(mingw)
        env.AppendENVPath('PATH', mingw_bin_dir)

    # Most of mingw is the same as gcc and friends...
    gnu_tools = ['gcc', 'g++', 'gnulink', 'ar', 'gas', 'gfortran', 'm4']
    for tool in gnu_tools:
        SCons.Tool.Tool(tool)(env)

    # ... but a few things differ:
    env['CC'] = 'gcc'
    # make sure the msvc tool doesnt break us, it added a /flag
    if 'CCFLAGS' in env:
        # make sure its a CLVar to handle list or str cases
        if type(env['CCFLAGS']) is not SCons.Util.CLVar:
            env['CCFLAGS'] = SCons.Util.CLVar(env['CCFLAGS'])
        env['CCFLAGS'] = SCons.Util.CLVar(str(env['CCFLAGS']).replace('/nologo', ''))
    env['SHCCFLAGS'] = SCons.Util.CLVar('$CCFLAGS')
    env['CXX'] = 'g++'
    env['SHCXXFLAGS'] = SCons.Util.CLVar('$CXXFLAGS')
    env['SHLINKFLAGS'] = SCons.Util.CLVar('$LINKFLAGS -shared')
    env['SHLINKCOM'] = shlib_action
    env['LDMODULECOM'] = ldmodule_action
    env.Append(SHLIBEMITTER=[shlib_emitter])
    env.Append(LDMODULEEMITTER=[shlib_emitter])
    env['AS'] = 'as'

    env['WIN32DEFPREFIX'] = ''
    env['WIN32DEFSUFFIX'] = '.def'
    env['WINDOWSDEFPREFIX'] = '${WIN32DEFPREFIX}'
    env['WINDOWSDEFSUFFIX'] = '${WIN32DEFSUFFIX}'

    env['SHOBJSUFFIX'] = '.o'
    env['STATIC_AND_SHARED_OBJECTS_ARE_THE_SAME'] = 1
    env['RC'] = 'windres'
    env['RCFLAGS'] = SCons.Util.CLVar('')
    env['RCINCFLAGS'] = '$( ${_concat(RCINCPREFIX, CPPPATH, RCINCSUFFIX, __env__, RDirs, TARGET, SOURCE)} $)'
    env['RCINCPREFIX'] = '--include-dir '
    env['RCINCSUFFIX'] = ''
    env['RCCOM'] = '$RC $_CPPDEFFLAGS $RCINCFLAGS ${RCINCPREFIX} ${SOURCE.dir} $RCFLAGS -i $SOURCE -o $TARGET'
    env['BUILDERS']['RES'] = res_builder

    # Some setting from the platform also have to be overridden:
    env['OBJSUFFIX'] = '.o'
    env['LIBPREFIX'] = 'lib'
    env['LIBSUFFIX'] = '.a'
    env['PROGSUFFIX'] = '.exe'


def exists(env):
    mingw = SCons.Tool.find_program_path(env, key_program, default_paths=mingw_paths)
    if mingw:
        mingw_bin_dir = os.path.dirname(mingw)
        env.AppendENVPath('PATH', mingw_bin_dir)

    return mingw

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
