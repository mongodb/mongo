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

from SCons.Tool import createLoadableModuleBuilder
from .SharedLibrary import shlib_symlink_emitter
from . import lib_emitter


def ldmod_symlink_emitter(target, source, env, **kw):
    return shlib_symlink_emitter(target, source, env, variable_prefix='LDMODULE')


def _get_ldmodule_stem(target, source, env, for_signature):
    """
    Get the basename for a library (so for libxyz.so, return xyz)
    :param target:
    :param source:
    :param env:
    :param for_signature:
    :return:
    """
    target_name = str(target)
    ldmodule_prefix = env.subst('$LDMODULEPREFIX')
    ldmodule_suffix = env.subst("$_LDMODULESUFFIX")

    if target_name.startswith(ldmodule_prefix):
        target_name = target_name[len(ldmodule_prefix):]

    if target_name.endswith(ldmodule_suffix):
        target_name = target_name[:-len(ldmodule_suffix)]

    return target_name


def _ldmodule_soversion(target, source, env, for_signature):
    """Function to determine what to use for SOVERSION"""

    if 'SOVERSION' in env:
        return '.$SOVERSION'
    elif 'LDMODULEVERSION' in env:
        ldmod_version = env.subst('$LDMODULEVERSION')
        # We use only the most significant digit of LDMODULEVERSION
        return '.' + ldmod_version.split('.')[0]
    else:
        return ''


def _ldmodule_soname(target, source, env, for_signature) -> str:
    if 'SONAME' in env:
        return '$SONAME'
    else:
        return "$LDMODULEPREFIX$_get_ldmodule_stem${LDMODULESUFFIX}$_LDMODULESOVERSION"

def _LDMODULEVERSION(target, source, env, for_signature):
    """
    Return "." + version if it's set, otherwise just a blank
    """
    value = env.subst('$LDMODULEVERSION', target=target, source=source)
    # print("_has_LDMODULEVERSION:%s"%value)
    if value:
        return "."+value
    else:
        return ""

def setup_loadable_module_logic(env) -> None:
    """
    Just the logic for loadable modules

    For most platforms, a loadable module is the same as a shared
    library.  Platforms which are different can override these, but
    setting them the same means that LoadableModule works everywhere.

    :param env:
    :return:
    """

    createLoadableModuleBuilder(env)

    env['_get_ldmodule_stem'] = _get_ldmodule_stem
    env['_LDMODULESOVERSION'] = _ldmodule_soversion
    env['_LDMODULESONAME'] = _ldmodule_soname

    env['LDMODULENAME'] = '${LDMODULEPREFIX}$_get_ldmodule_stem${_LDMODULESUFFIX}'

    # This is the non versioned LDMODULE filename
    # If LDMODULEVERSION is defined then this will symlink to $LDMODULENAME
    env['LDMODULE_NOVERSION_SYMLINK'] = '$_get_shlib_dir${LDMODULEPREFIX}$_get_ldmodule_stem${LDMODULESUFFIX}'

    # This is the sonamed file name
    # If LDMODULEVERSION is defined then this will symlink to $LDMODULENAME
    env['LDMODULE_SONAME_SYMLINK'] = '$_get_shlib_dir$_LDMODULESONAME'

    env['_LDMODULEVERSION'] =  _LDMODULEVERSION

    env['_LDMODULEVERSIONFLAGS'] = '$LDMODULEVERSIONFLAGS -Wl,-soname=$_LDMODULESONAME'

    env['LDMODULEEMITTER'] = [lib_emitter, ldmod_symlink_emitter]

    env['LDMODULEPREFIX'] = '$SHLIBPREFIX'
    env['_LDMODULESUFFIX'] = '${LDMODULESUFFIX}${_LDMODULEVERSION}'
    env['LDMODULESUFFIX'] = '$SHLIBSUFFIX'

    env['LDMODULE'] = '$SHLINK'

    env['LDMODULEFLAGS'] = '$SHLINKFLAGS'

    env['LDMODULECOM'] = '$LDMODULE -o $TARGET $LDMODULEFLAGS $__LDMODULEVERSIONFLAGS $__RPATH $SOURCES ' \
                         '$_LIBDIRFLAGS $_LIBFLAGS '

    env['LDMODULEVERSION'] = '$SHLIBVERSION'
    env['LDMODULENOVERSIONSYMLINKS'] = '$SHLIBNOVERSIONSYMLINKS'