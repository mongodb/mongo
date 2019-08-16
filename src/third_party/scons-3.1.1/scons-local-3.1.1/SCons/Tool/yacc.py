"""SCons.Tool.yacc

Tool-specific initialization for yacc.

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

__revision__ = "src/engine/SCons/Tool/yacc.py 72ae09dc35ac2626f8ff711d8c4b30b6138e08e3 2019-08-08 14:50:06 bdeegan"

import os.path
import sys

import SCons.Defaults
import SCons.Tool
import SCons.Util
from SCons.Platform.mingw import MINGW_DEFAULT_PATHS
from SCons.Platform.cygwin import CYGWIN_DEFAULT_PATHS
from SCons.Platform.win32 import CHOCO_DEFAULT_PATH

YaccAction = SCons.Action.Action("$YACCCOM", "$YACCCOMSTR")

if sys.platform == 'win32':
    BINS = ['bison', 'yacc', 'win_bison']
else:
    BINS = ["bison", "yacc"]

def _yaccEmitter(target, source, env, ysuf, hsuf):
    yaccflags = env.subst("$YACCFLAGS", target=target, source=source)
    flags = SCons.Util.CLVar(yaccflags)
    targetBase, targetExt = os.path.splitext(SCons.Util.to_String(target[0]))

    if '.ym' in ysuf:                # If using Objective-C
        target = [targetBase + ".m"] # the extension is ".m".


    # If -d is specified on the command line, yacc will emit a .h
    # or .hpp file with the same name as the .c or .cpp output file.
    if '-d' in flags:
        target.append(targetBase + env.subst(hsuf, target=target, source=source))

    # If -g is specified on the command line, yacc will emit a .vcg
    # file with the same base name as the .y, .yacc, .ym or .yy file.
    if "-g" in flags:
        base, ext = os.path.splitext(SCons.Util.to_String(source[0]))
        target.append(base + env.subst("$YACCVCGFILESUFFIX"))

    # If -v is specified yacc will create the output debug file
    # which is not really source for any process, but should
    # be noted and also be cleaned
    # Bug #2558
    if "-v" in flags:
        env.SideEffect(targetBase+'.output',target[0])
        env.Clean(target[0],targetBase+'.output')



    # With --defines and --graph, the name of the file is totally defined
    # in the options.
    fileGenOptions = ["--defines=", "--graph="]
    for option in flags:
        for fileGenOption in fileGenOptions:
            l = len(fileGenOption)
            if option[:l] == fileGenOption:
                # A file generating option is present, so add the file
                # name to the list of targets.
                fileName = option[l:].strip()
                target.append(fileName)

    return (target, source)

def yEmitter(target, source, env):
    return _yaccEmitter(target, source, env, ['.y', '.yacc'], '$YACCHFILESUFFIX')

def ymEmitter(target, source, env):
    return _yaccEmitter(target, source, env, ['.ym'], '$YACCHFILESUFFIX')

def yyEmitter(target, source, env):
    return _yaccEmitter(target, source, env, ['.yy'], '$YACCHXXFILESUFFIX')

def get_yacc_path(env, append_paths=False):
    """
    Find the path to the yacc tool, searching several possible names

    Only called in the Windows case, so the default_path
    can be Windows-specific

    :param env: current construction environment
    :param append_paths: if set, add the path to the tool to PATH
    :return: path to yacc tool, if found
    """
    for prog in BINS:
        bin_path = SCons.Tool.find_program_path(
            env,
            prog,
            default_paths=CHOCO_DEFAULT_PATH + MINGW_DEFAULT_PATHS + CYGWIN_DEFAULT_PATHS )
        if bin_path:
            if append_paths:
                env.AppendENVPath('PATH', os.path.dirname(bin_path))
            return bin_path
    SCons.Warnings.Warning('yacc tool requested, but yacc or bison binary not found in ENV PATH')


def generate(env):
    """Add Builders and construction variables for yacc to an Environment."""
    c_file, cxx_file = SCons.Tool.createCFileBuilders(env)

    # C
    c_file.add_action('.y', YaccAction)
    c_file.add_emitter('.y', yEmitter)

    c_file.add_action('.yacc', YaccAction)
    c_file.add_emitter('.yacc', yEmitter)

    # Objective-C
    c_file.add_action('.ym', YaccAction)
    c_file.add_emitter('.ym', ymEmitter)

    # C++
    cxx_file.add_action('.yy', YaccAction)
    cxx_file.add_emitter('.yy', yyEmitter)

    if sys.platform == 'win32':
        # ignore the return, all we need is for the path to be added
        _ = get_yacc_path(env, append_paths=True)

    env["YACC"] = env.Detect(BINS)
    env['YACCFLAGS'] = SCons.Util.CLVar('')
    env['YACCCOM']   = '$YACC $YACCFLAGS -o $TARGET $SOURCES'
    env['YACCHFILESUFFIX'] = '.h'
    env['YACCHXXFILESUFFIX'] = '.hpp'
    env['YACCVCGFILESUFFIX'] = '.vcg'

def exists(env):
    if sys.platform == 'win32':
        return get_yacc_path(env)
    else:
        return env.Detect(BINS)

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
