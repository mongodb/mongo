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

"""SCons.Tool.DCommon

Common code for the various D tools.

Coded by Russel Winder (russel@winder.org.uk)
2012-09-06
"""

from pathlib import Path
import os.path


def isD(env, source) -> bool:
    if not source:
        return False
    for s in source:
        if s.sources:
            ext = os.path.splitext(str(s.sources[0]))[1]
            if ext == '.d':
                return True
    return False


def addDPATHToEnv(env, executable) -> None:
    dPath = env.WhereIs(executable)
    if dPath:
        phobosDir = dPath[:dPath.rindex(executable)] + '/../src/phobos'
        if os.path.isdir(phobosDir):
            env.Append(DPATH=[phobosDir])


def allAtOnceEmitter(target, source, env):
    if env['DC'] in ('ldc2', 'dmd'):
        env.SideEffect(str(target[0]) + '.o', target[0])
        env.Clean(target[0], str(target[0]) + '.o')
    return target, source

def DObjectEmitter(target,source,env):
    di_file_dir = env.get('DI_FILE_DIR', False)
    # TODO: Verify sane DI_FILE_DIR?
    if di_file_dir:
        di_file_suffix = env.subst('$DI_FILE_SUFFIX', target=target, source=source)
        file_base = Path(target[0].get_path()).stem
        # print(f'DObjectEmitter: {di_file_dir}/*{file_base}*{di_file_suffix}')
        target.append(env.fs.File(f"{file_base}{di_file_suffix}", di_file_dir))
        # print("New Target:%s"%" ".join([str(t) for t in target]))
    return (target,source)

def DStaticObjectEmitter(target,source,env):
    for tgt in target:
        tgt.attributes.shared = None
    return DObjectEmitter(target,source,env)

def DSharedObjectEmitter(target,source,env):
    for tgt in target:
        tgt.attributes.shared = 1
    return DObjectEmitter(target,source,env)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
