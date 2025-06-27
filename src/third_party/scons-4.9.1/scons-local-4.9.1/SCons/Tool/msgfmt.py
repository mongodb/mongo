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

""" msgfmt tool """

import sys
import os

import SCons.Action
import SCons.Tool
import SCons.Util
import SCons.Warnings
from SCons.Builder import BuilderBase
from SCons.Errors import StopError
from SCons.Platform.cygwin import CYGWIN_DEFAULT_PATHS
from SCons.Platform.mingw import MINGW_DEFAULT_PATHS
from SCons.Tool.GettextCommon import (
    _detect_msgfmt,
    _msgfmt_exists,
    # MsgfmtToolWarning,
    _read_linguas_from_files,
)

class _MOFileBuilder(BuilderBase):
    """The builder class for `MO` files.

    The reason for this builder to exists and its purpose is quite simillar
    as for `_POFileBuilder`. This time, we extend list of sources, not targets,
    and call `BuilderBase._execute()` only once (as we assume single-target
    here).
    """

    def _execute(self, env, target, source, *args, **kw):
        # Here we add support for 'LINGUAS_FILE' keyword. Emitter is not suitable
        # in this case, as it is called too late (after multiple sources
        # are handled single_source builder.

        linguas_files = None
        if 'LINGUAS_FILE' in env and env['LINGUAS_FILE'] is not None:
            linguas_files = env['LINGUAS_FILE']
            # This should prevent from endless recursion.
            env['LINGUAS_FILE'] = None
            # We read only languages. Suffixes shall be added automatically.
            linguas = _read_linguas_from_files(env, linguas_files)
            if SCons.Util.is_List(source):
                source.extend(linguas)
            elif source is not None:
                source = [source] + linguas
            else:
                source = linguas
        result = BuilderBase._execute(self, env, target, source, *args, **kw)
        if linguas_files is not None:
            env['LINGUAS_FILE'] = linguas_files
        return result


def _create_mo_file_builder(env, **kw):
    """ Create builder object for `MOFiles` builder """

    # FIXME: What factory use for source? Ours or their?
    kw['action'] = SCons.Action.Action('$MSGFMTCOM', '$MSGFMTCOMSTR')
    kw['suffix'] = '$MOSUFFIX'
    kw['src_suffix'] = '$POSUFFIX'
    kw['src_builder'] = '_POUpdateBuilder'
    kw['single_source'] = True
    return _MOFileBuilder(**kw)


def generate(env, **kw) -> None:
    """ Generate `msgfmt` tool """

    if sys.platform == 'win32':
        msgfmt = SCons.Tool.find_program_path(
            env, 'msgfmt', default_paths=MINGW_DEFAULT_PATHS + CYGWIN_DEFAULT_PATHS
        )
        if msgfmt:
            msgfmt_bin_dir = os.path.dirname(msgfmt)
            env.AppendENVPath('PATH', msgfmt_bin_dir)
        else:
            SCons.Warnings.warn(
                # MsgfmtToolWarning,  # using this breaks test, so keep:
                SCons.Warnings.SConsWarning,
                'msgfmt tool requested, but binary not found in ENV PATH',
            )

    try:
        env['MSGFMT'] = _detect_msgfmt(env)
    except StopError:
        env['MSGFMT'] = 'msgfmt'
    env.SetDefault(
        MSGFMTFLAGS=[SCons.Util.CLVar('-c')],
        MSGFMTCOM='$MSGFMT $MSGFMTFLAGS -o $TARGET $SOURCE',
        MSGFMTCOMSTR='',
        MOSUFFIX=['.mo'],
        POSUFFIX=['.po'],
    )
    env.Append(BUILDERS={'MOFiles': _create_mo_file_builder(env)})


def exists(env):
    """ Check if the tool exists """

    try:
        return _msgfmt_exists(env)
    except StopError:
        return False

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
