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

"""Tool specific initialization of msginit tool."""

import sys
import os

import SCons.Action
import SCons.Tool
import SCons.Util
import SCons.Warnings
from SCons.Environment import _null
from SCons.Errors import StopError
from SCons.Platform.cygwin import CYGWIN_DEFAULT_PATHS
from SCons.Platform.mingw import MINGW_DEFAULT_PATHS
from SCons.Tool.GettextCommon import (
    _detect_msginit,
    _init_po_files,
    _msginit_exists,
    # MsginitToolWarning,
    _POFileBuilder,
)


def _optional_no_translator_flag(env):
    """Return '--no-translator' flag if we run *msginit(1)*  in non-interactive
    mode."""
    if 'POAUTOINIT' in env:
        autoinit = env['POAUTOINIT']
    else:
        autoinit = False
    if autoinit:
        return [SCons.Util.CLVar('--no-translator')]
    else:
        return [SCons.Util.CLVar('')]


def _POInitBuilder(env, **kw):
    """ Create builder object for `POInit` builder. """

    action = SCons.Action.Action(_init_po_files, None)
    return _POFileBuilder(env, action=action, target_alias='$POCREATE_ALIAS')


def _POInitBuilderWrapper(env, target=None, source=_null, **kw):
    """Wrapper for _POFileBuilder. We use it to make user's life easier.

    This wrapper checks for `$POTDOMAIN` construction variable (or override in
    `**kw`) and treats it appropriatelly.
    """
    if source is _null:
        if 'POTDOMAIN' in kw:
            domain = kw['POTDOMAIN']
        elif 'POTDOMAIN' in env:
            domain = env['POTDOMAIN']
        else:
            domain = 'messages'
        source = [domain]  # NOTE: Suffix shall be appended automatically
    return env._POInitBuilder(target, source, **kw)

def generate(env, **kw) -> None:
    """ Generate the `msginit` tool """

    if sys.platform == 'win32':
        msginit = SCons.Tool.find_program_path(
            env, 'msginit', default_paths=MINGW_DEFAULT_PATHS + CYGWIN_DEFAULT_PATHS
        )
        if msginit:
            msginit_bin_dir = os.path.dirname(msginit)
            env.AppendENVPath('PATH', msginit_bin_dir)
        else:
            SCons.Warnings.warn(
                # MsginitToolWarning,  # using this breaks test, so keep:
                SCons.Warnings.SConsWarning,
                'msginit tool requested, but binary not found in ENV PATH',
            )

    try:
        env['MSGINIT'] = _detect_msginit(env)
    except StopError:
        env['MSGINIT'] = 'msginit'
    msginitcom = (
        '$MSGINIT ${_MSGNoTranslator(__env__)} -l ${_MSGINITLOCALE}'
        + ' $MSGINITFLAGS -i $SOURCE -o $TARGET'
    )
    # NOTE: We set POTSUFFIX here, in case the 'xgettext' is not loaded
    #       (sometimes we really don't need it)
    env.SetDefault(
        POSUFFIX=['.po'],
        POTSUFFIX=['.pot'],
        _MSGINITLOCALE='${TARGET.filebase}',
        _MSGNoTranslator=_optional_no_translator_flag,
        MSGINITCOM=msginitcom,
        MSGINITCOMSTR='',
        MSGINITFLAGS=[],
        POAUTOINIT=False,
        POCREATE_ALIAS='po-create',
    )
    env.Append(BUILDERS={'_POInitBuilder': _POInitBuilder(env)})
    env.AddMethod(_POInitBuilderWrapper, 'POInit')
    env.AlwaysBuild(env.Alias('$POCREATE_ALIAS'))


def exists(env):
    """ Check if the tool exists """

    try:
        return _msginit_exists(env)
    except StopError:
        return False

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
