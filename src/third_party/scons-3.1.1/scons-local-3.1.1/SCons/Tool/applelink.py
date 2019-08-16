"""SCons.Tool.applelink

Tool-specific initialization for Apple's gnu-like linker.

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

__revision__ = "src/engine/SCons/Tool/applelink.py 72ae09dc35ac2626f8ff711d8c4b30b6138e08e3 2019-08-08 14:50:06 bdeegan"

import SCons.Util

# Even though the Mac is based on the GNU toolchain, it doesn't understand
# the -rpath option, so we use the "link" tool instead of "gnulink".
from . import link


class AppleLinkInvalidCurrentVersionException(Exception):
    pass

class AppleLinkInvalidCompatibilityVersionException(Exception):
    pass


def _applelib_versioned_lib_suffix(env, suffix, version):
    """For suffix='.dylib' and version='0.1.2' it returns '.0.1.2.dylib'"""
    Verbose = False
    if Verbose:
        print("_applelib_versioned_lib_suffix: suffix={!r}".format(suffix))
        print("_applelib_versioned_lib_suffix: version={!r}".format(version))
    if version not in suffix:
        suffix = "." + version + suffix
    if Verbose:
        print("_applelib_versioned_lib_suffix: return suffix={!r}".format(suffix))
    return suffix


def _applelib_versioned_lib_soname(env, libnode, version, prefix, suffix, name_func):
    """For libnode='/optional/dir/libfoo.X.Y.Z.dylib' it returns 'libfoo.X.dylib'"""
    Verbose = False
    if Verbose:
        print("_applelib_versioned_lib_soname: version={!r}".format(version))
    name = name_func(env, libnode, version, prefix, suffix)
    if Verbose:
        print("_applelib_versioned_lib_soname: name={!r}".format(name))
    major = version.split('.')[0]
    (libname,_suffix) = name.split('.')
    soname = '.'.join([libname, major, _suffix])
    if Verbose:
        print("_applelib_versioned_lib_soname: soname={!r}".format(soname))
    return soname

def _applelib_versioned_shlib_soname(env, libnode, version, prefix, suffix):
    return _applelib_versioned_lib_soname(env, libnode, version, prefix, suffix, link._versioned_shlib_name)


# User programmatically describes how SHLIBVERSION maps to values for compat/current.
_applelib_max_version_values = (65535, 255, 255)
def _applelib_check_valid_version(version_string):
    """
    Check that the version # is valid.
    X[.Y[.Z]]
    where X 0-65535
    where Y either not specified or 0-255
    where Z either not specified or 0-255
    :param version_string:
    :return:
    """
    parts = version_string.split('.')
    if len(parts) > 3:
        return False, "Version string has too many periods [%s]"%version_string
    if len(parts) <= 0:
        return False, "Version string unspecified [%s]"%version_string

    for (i, p) in enumerate(parts):
        try:
            p_i = int(p)
        except ValueError:
            return False, "Version component %s (from %s) is not a number"%(p, version_string)
        if p_i < 0 or p_i > _applelib_max_version_values[i]:
            return False, "Version component %s (from %s) is not valid value should be between 0 and %d"%(p, version_string, _applelib_max_version_values[i])

    return True, ""


def _applelib_currentVersionFromSoVersion(source, target, env, for_signature):
    """
    A generator function to create the -Wl,-current_version flag if needed.
    If env['APPLELINK_NO_CURRENT_VERSION'] contains a true value no flag will be generated
    Otherwise if APPLELINK_CURRENT_VERSION is not specified, env['SHLIBVERSION']
    will be used.

    :param source:
    :param target:
    :param env:
    :param for_signature:
    :return: A string providing the flag to specify the current_version of the shared library
    """
    if env.get('APPLELINK_NO_CURRENT_VERSION', False):
        return ""
    elif env.get('APPLELINK_CURRENT_VERSION', False):
        version_string = env['APPLELINK_CURRENT_VERSION']
    elif env.get('SHLIBVERSION', False):
        version_string = env['SHLIBVERSION']
    else:
        return ""

    version_string = ".".join(version_string.split('.')[:3])

    valid, reason = _applelib_check_valid_version(version_string)
    if not valid:
        raise AppleLinkInvalidCurrentVersionException(reason)

    return "-Wl,-current_version,%s" % version_string


def _applelib_compatVersionFromSoVersion(source, target, env, for_signature):
    """
    A generator function to create the -Wl,-compatibility_version flag if needed.
    If env['APPLELINK_NO_COMPATIBILITY_VERSION'] contains a true value no flag will be generated
    Otherwise if APPLELINK_COMPATIBILITY_VERSION is not specified
    the first two parts of env['SHLIBVERSION'] will be used with a .0 appended.

    :param source:
    :param target:
    :param env:
    :param for_signature:
    :return: A string providing the flag to specify the compatibility_version of the shared library
    """
    if env.get('APPLELINK_NO_COMPATIBILITY_VERSION', False):
        return ""
    elif env.get('APPLELINK_COMPATIBILITY_VERSION', False):
        version_string = env['APPLELINK_COMPATIBILITY_VERSION']
    elif env.get('SHLIBVERSION', False):
        version_string = ".".join(env['SHLIBVERSION'].split('.')[:2] + ['0'])
    else:
        return ""

    if version_string is None:
        return ""

    valid, reason = _applelib_check_valid_version(version_string)
    if not valid:
        raise AppleLinkInvalidCompatibilityVersionException(reason)

    return "-Wl,-compatibility_version,%s" % version_string


def generate(env):
    """Add Builders and construction variables for applelink to an
    Environment."""
    link.generate(env)

    env['FRAMEWORKPATHPREFIX'] = '-F'
    env['_FRAMEWORKPATH'] = '${_concat(FRAMEWORKPATHPREFIX, FRAMEWORKPATH, "", __env__, RDirs)}'

    env['_FRAMEWORKS'] = '${_concat("-framework ", FRAMEWORKS, "", __env__)}'
    env['LINKCOM'] = env['LINKCOM'] + ' $_FRAMEWORKPATH $_FRAMEWORKS $FRAMEWORKSFLAGS'
    env['SHLINKFLAGS'] = SCons.Util.CLVar('$LINKFLAGS -dynamiclib')
    env['SHLINKCOM'] = env['SHLINKCOM'] + ' $_FRAMEWORKPATH $_FRAMEWORKS $FRAMEWORKSFLAGS'


    # see: http://docstore.mik.ua/orelly/unix3/mac/ch05_04.htm  for proper naming
    link._setup_versioned_lib_variables(env, tool = 'applelink')#, use_soname = use_soname)
    env['LINKCALLBACKS'] = link._versioned_lib_callbacks()
    env['LINKCALLBACKS']['VersionedShLibSuffix'] = _applelib_versioned_lib_suffix
    env['LINKCALLBACKS']['VersionedShLibSoname'] = _applelib_versioned_shlib_soname

    env['_APPLELINK_CURRENT_VERSION'] = _applelib_currentVersionFromSoVersion
    env['_APPLELINK_COMPATIBILITY_VERSION'] = _applelib_compatVersionFromSoVersion
    env['_SHLIBVERSIONFLAGS'] = '$_APPLELINK_CURRENT_VERSION $_APPLELINK_COMPATIBILITY_VERSION '
    env['_LDMODULEVERSIONFLAGS'] = '$_APPLELINK_CURRENT_VERSION $_APPLELINK_COMPATIBILITY_VERSION '

    # override the default for loadable modules, which are different
    # on OS X than dynamic shared libs.  echoing what XCode does for
    # pre/suffixes:
    env['LDMODULEPREFIX'] = '' 
    env['LDMODULESUFFIX'] = '' 
    env['LDMODULEFLAGS'] = SCons.Util.CLVar('$LINKFLAGS -bundle')
    env['LDMODULECOM'] = '$LDMODULE -o ${TARGET} $LDMODULEFLAGS $SOURCES $_LIBDIRFLAGS $_LIBFLAGS $_FRAMEWORKPATH $_FRAMEWORKS $FRAMEWORKSFLAGS'

    env['__SHLIBVERSIONFLAGS'] = '${__libversionflags(__env__,"SHLIBVERSION","_SHLIBVERSIONFLAGS")}'



def exists(env):
    return env['PLATFORM'] == 'darwin'

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
