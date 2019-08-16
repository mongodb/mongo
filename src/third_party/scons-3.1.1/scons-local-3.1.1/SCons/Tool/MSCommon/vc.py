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

# TODO:
#   * supported arch for versions: for old versions of batch file without
#     argument, giving bogus argument cannot be detected, so we have to hardcode
#     this here
#   * print warning when msvc version specified but not found
#   * find out why warning do not print
#   * test on 64 bits XP +  VS 2005 (and VS 6 if possible)
#   * SDK
#   * Assembly
__revision__ = "src/engine/SCons/Tool/MSCommon/vc.py 72ae09dc35ac2626f8ff711d8c4b30b6138e08e3 2019-08-08 14:50:06 bdeegan"

__doc__ = """Module for Visual C/C++ detection and configuration.
"""
import SCons.compat
import SCons.Util

import subprocess
import os
import platform
from string import digits as string_digits

import SCons.Warnings
from SCons.Tool import find_program_path

from . import common

debug = common.debug

from . import sdk

get_installed_sdks = sdk.get_installed_sdks


class VisualCException(Exception):
    pass

class UnsupportedVersion(VisualCException):
    pass

class MSVCUnsupportedHostArch(VisualCException):
    pass

class MSVCUnsupportedTargetArch(VisualCException):
    pass

class MissingConfiguration(VisualCException):
    pass

class NoVersionFound(VisualCException):
    pass

class BatchFileExecutionError(VisualCException):
    pass

# Dict to 'canonalize' the arch
_ARCH_TO_CANONICAL = {
    "amd64"     : "amd64",
    "emt64"     : "amd64",
    "i386"      : "x86",
    "i486"      : "x86",
    "i586"      : "x86",
    "i686"      : "x86",
    "ia64"      : "ia64",      # deprecated
    "itanium"   : "ia64",      # deprecated
    "x86"       : "x86",
    "x86_64"    : "amd64",
    "arm"       : "arm",
    "arm64"     : "arm64",
    "aarch64"   : "arm64",
}

_HOST_TARGET_TO_CL_DIR_GREATER_THAN_14 = {
    ("amd64","amd64")  : ("Hostx64","x64"),
    ("amd64","x86")    : ("Hostx64","x86"),
    ("amd64","arm")    : ("Hostx64","arm"),
    ("amd64","arm64")  : ("Hostx64","arm64"),
    ("x86","amd64")    : ("Hostx86","x64"),
    ("x86","x86")      : ("Hostx86","x86"),
    ("x86","arm")      : ("Hostx86","arm"),
    ("x86","arm64")    : ("Hostx86","arm64"),
}

# get path to the cl.exe dir for older VS versions
# based off a tuple of (host, target) platforms
_HOST_TARGET_TO_CL_DIR = {
    ("amd64","amd64")  : "amd64",
    ("amd64","x86")    : "amd64_x86",
    ("amd64","arm")    : "amd64_arm",
    ("amd64","arm64")  : "amd64_arm64",
    ("x86","amd64")    : "x86_amd64",
    ("x86","x86")      : "",
    ("x86","arm")      : "x86_arm",
    ("x86","arm64")    : "x86_arm64",
}

# Given a (host, target) tuple, return the argument for the bat file.
# Both host and targets should be canonalized.
_HOST_TARGET_ARCH_TO_BAT_ARCH = {
    ("x86", "x86"): "x86",
    ("x86", "amd64"): "x86_amd64",
    ("x86", "x86_amd64"): "x86_amd64",
    ("amd64", "x86_amd64"): "x86_amd64", # This is present in (at least) VS2012 express
    ("amd64", "amd64"): "amd64",
    ("amd64", "x86"): "x86",
    ("x86", "ia64"): "x86_ia64",         # gone since 14.0
    ("arm", "arm"): "arm",              # since 14.0, maybe gone 14.1?
    ("x86", "arm"): "x86_arm",          # since 14.0
    ("x86", "arm64"): "x86_arm64",      # since 14.1
    ("amd64", "arm"): "amd64_arm",      # since 14.0
    ("amd64", "arm64"): "amd64_arm64",  # since 14.1
}

_CL_EXE_NAME = 'cl.exe'

def get_msvc_version_numeric(msvc_version):
    """Get the raw version numbers from a MSVC_VERSION string, so it
    could be cast to float or other numeric values. For example, '14.0Exp'
    would get converted to '14.0'.

    Args:
        msvc_version: str
            string representing the version number, could contain non
            digit characters

    Returns:
        str: the value converted to a numeric only string

    """
    return ''.join([x for  x in msvc_version if x in string_digits + '.'])

def get_host_target(env):
    debug('vc.py:get_host_target()')

    host_platform = env.get('HOST_ARCH')
    if not host_platform:
        host_platform = platform.machine()
        # TODO(2.5):  the native Python platform.machine() function returns
        # '' on all Python versions before 2.6, after which it also uses
        # PROCESSOR_ARCHITECTURE.
        if not host_platform:
            host_platform = os.environ.get('PROCESSOR_ARCHITECTURE', '')

    # Retain user requested TARGET_ARCH
    req_target_platform = env.get('TARGET_ARCH')
    debug('vc.py:get_host_target() req_target_platform:%s'%req_target_platform)

    if  req_target_platform:
        # If user requested a specific platform then only try that one.
        target_platform = req_target_platform
    else:
        target_platform = host_platform

    try:
        host = _ARCH_TO_CANONICAL[host_platform.lower()]
    except KeyError:
        msg = "Unrecognized host architecture %s"
        raise MSVCUnsupportedHostArch(msg % repr(host_platform))

    try:
        target = _ARCH_TO_CANONICAL[target_platform.lower()]
    except KeyError:
        all_archs = str(list(_ARCH_TO_CANONICAL.keys()))
        raise MSVCUnsupportedTargetArch("Unrecognized target architecture %s\n\tValid architectures: %s" % (target_platform, all_archs))

    return (host, target,req_target_platform)

# If you update this, update SupportedVSList in Tool/MSCommon/vs.py, and the
# MSVC_VERSION documentation in Tool/msvc.xml.
_VCVER = ["14.2", "14.1", "14.0", "14.0Exp", "12.0", "12.0Exp", "11.0", "11.0Exp", "10.0", "10.0Exp", "9.0", "9.0Exp","8.0", "8.0Exp","7.1", "7.0", "6.0"]

# if using vswhere, a further mapping is needed
_VCVER_TO_VSWHERE_VER = {
    '14.2' : '[16.0, 17.0)',
    '14.1' : '[15.0, 16.0)',
}

_VCVER_TO_PRODUCT_DIR = {
    '14.2' : [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'')], # VS 2019 doesn't set this key
    '14.1' : [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'')], # VS 2017 doesn't set this key
    '14.0' : [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\14.0\Setup\VC\ProductDir')],
    '14.0Exp' : [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\14.0\Setup\VC\ProductDir')],
    '12.0' : [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\12.0\Setup\VC\ProductDir'),
        ],
    '12.0Exp' : [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\12.0\Setup\VC\ProductDir'),
        ],
    '11.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\11.0\Setup\VC\ProductDir'),
        ],
    '11.0Exp' : [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\11.0\Setup\VC\ProductDir'),
        ],
    '10.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\10.0\Setup\VC\ProductDir'),
        ],
    '10.0Exp' : [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\10.0\Setup\VC\ProductDir'),
        ],
    '9.0': [
        (SCons.Util.HKEY_CURRENT_USER, r'Microsoft\DevDiv\VCForPython\9.0\installdir',),
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\9.0\Setup\VC\ProductDir',),
        ],
    '9.0Exp' : [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\9.0\Setup\VC\ProductDir'),
        ],
    '8.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\8.0\Setup\VC\ProductDir'),
        ],
    '8.0Exp': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\8.0\Setup\VC\ProductDir'),
        ],
    '7.1': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\7.1\Setup\VC\ProductDir'),
        ],
    '7.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\7.0\Setup\VC\ProductDir'),
        ],
    '6.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\6.0\Setup\Microsoft Visual C++\ProductDir'),
        ]
}

def msvc_version_to_maj_min(msvc_version):
    msvc_version_numeric = get_msvc_version_numeric(msvc_version)

    t = msvc_version_numeric.split(".")
    if not len(t) == 2:
        raise ValueError("Unrecognized version %s (%s)" % (msvc_version,msvc_version_numeric))
    try:
        maj = int(t[0])
        min = int(t[1])
        return maj, min
    except ValueError as e:
        raise ValueError("Unrecognized version %s (%s)" % (msvc_version,msvc_version_numeric))

def is_host_target_supported(host_target, msvc_version):
    """Check if (host, target) pair is supported for a VC version.

    :note: only checks whether a given version *may* support the given (host,
        target), not that the toolchain is actually present on the machine.
    :param tuple host_target: canonalized host-targets pair, e.g.
        ("x86", "amd64") for cross compilation from 32 bit Windows to 64 bits.
    :param str msvc_version: Visual C++ version (major.minor), e.g. "10.0"
    :returns: True or False
    """
    # We assume that any Visual Studio version supports x86 as a target
    if host_target[1] != "x86":
        maj, min = msvc_version_to_maj_min(msvc_version)
        if maj < 8:
            return False
    return True


def find_vc_pdir_vswhere(msvc_version):
    """
    Find the MSVC product directory using the vswhere program.

    :param msvc_version: MSVC version to search for
    :return: MSVC install dir or None
    :raises UnsupportedVersion: if the version is not known by this file
    """

    try:
        vswhere_version = _VCVER_TO_VSWHERE_VER[msvc_version]
    except KeyError:
        debug("Unknown version of MSVC: %s" % msvc_version)
        raise UnsupportedVersion("Unknown version %s" % msvc_version)

    # For bug 3333 - support default location of vswhere for both 64 and 32 bit windows
    # installs.
    for pf in ['Program Files (x86)', 'Program Files']:
        vswhere_path = os.path.join(
            'C:\\',
            pf,
            'Microsoft Visual Studio',
            'Installer',
            'vswhere.exe'
        )
        if os.path.exists(vswhere_path):
            # If we found vswhere, then use it.
            break
    else:
        # No vswhere on system, no install info available
        return None

    vswhere_cmd = [vswhere_path,
                   '-products', '*',
                   '-version', vswhere_version,
                   '-property', 'installationPath']

    #TODO PY27 cannot use Popen as context manager
    # try putting it back to the old way for now
    sp = subprocess.Popen(vswhere_cmd,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    vsdir, err = sp.communicate()
    if vsdir:
        vsdir = vsdir.decode("mbcs").splitlines()
        # vswhere could easily return multiple lines
        # we could define a way to pick the one we prefer, but since
        # this data is currently only used to make a check for existence,
        # returning the first hit should be good enough for now.
        vc_pdir = os.path.join(vsdir[0], 'VC')
        return vc_pdir
    else:
        # No vswhere on system, no install info available
        return None


def find_vc_pdir(msvc_version):
    """Find the MSVC product directory for the given version.

    Tries to look up the path using a registry key from the table
    _VCVER_TO_PRODUCT_DIR; if there is no key, calls find_vc_pdir_wshere
    for help instead.

    Args:
        msvc_version: str
            msvc version (major.minor, e.g. 10.0)

    Returns:
        str: Path found in registry, or None

    Raises:
        UnsupportedVersion: if the version is not known by this file.
        MissingConfiguration: found version but the directory is missing.

        Both exceptions inherit from VisualCException.
    """
    root = 'Software\\'
    try:
        hkeys = _VCVER_TO_PRODUCT_DIR[msvc_version]
    except KeyError:
        debug("Unknown version of MSVC: %s" % msvc_version)
        raise UnsupportedVersion("Unknown version %s" % msvc_version)

    for hkroot, key in hkeys:
        try:
            comps = None
            if not key:
                comps = find_vc_pdir_vswhere(msvc_version)
                if not comps:
                    debug('find_vc_pdir_vswhere(): no VC found for version {}'.format(repr(msvc_version)))
                    raise SCons.Util.WinError
                debug('find_vc_pdir_vswhere(): VC found: {}'.format(repr(msvc_version)))
                return comps
            else:
                if common.is_win64():
                    try:
                        # ordinally at win64, try Wow6432Node first.
                        comps = common.read_reg(root + 'Wow6432Node\\' + key, hkroot)
                    except SCons.Util.WinError as e:
                        # at Microsoft Visual Studio for Python 2.7, value is not in Wow6432Node
                        pass
                if not comps:
                    # not Win64, or Microsoft Visual Studio for Python 2.7
                    comps = common.read_reg(root + key, hkroot)
        except SCons.Util.WinError as e:
            debug('find_vc_dir(): no VC registry key {}'.format(repr(key)))
        else:
            debug('find_vc_dir(): found VC in registry: {}'.format(comps))
            if os.path.exists(comps):
                return comps
            else:
                debug('find_vc_dir(): reg says dir is {}, but it does not exist. (ignoring)'.format(comps))
                raise MissingConfiguration("registry dir {} not found on the filesystem".format(comps))
    return None

def find_batch_file(env,msvc_version,host_arch,target_arch):
    """
    Find the location of the batch script which should set up the compiler
    for any TARGET_ARCH whose compilers were installed by Visual Studio/VCExpress
    """
    pdir = find_vc_pdir(msvc_version)
    if pdir is None:
        raise NoVersionFound("No version of Visual Studio found")

    debug('vc.py: find_batch_file() in {}'.format(pdir))

    # filter out e.g. "Exp" from the version name
    msvc_ver_numeric = get_msvc_version_numeric(msvc_version)
    vernum = float(msvc_ver_numeric)
    if 7 <= vernum < 8:
        pdir = os.path.join(pdir, os.pardir, "Common7", "Tools")
        batfilename = os.path.join(pdir, "vsvars32.bat")
    elif vernum < 7:
        pdir = os.path.join(pdir, "Bin")
        batfilename = os.path.join(pdir, "vcvars32.bat")
    elif 8 <= vernum <= 14:
        batfilename = os.path.join(pdir, "vcvarsall.bat")
    else:  # vernum >= 14.1  VS2017 and above
        batfilename = os.path.join(pdir, "Auxiliary", "Build", "vcvarsall.bat")

    if not os.path.exists(batfilename):
        debug("Not found: %s" % batfilename)
        batfilename = None

    installed_sdks=get_installed_sdks()
    for _sdk in installed_sdks:
        sdk_bat_file = _sdk.get_sdk_vc_script(host_arch,target_arch)
        if not sdk_bat_file:
            debug("vc.py:find_batch_file() not found:%s"%_sdk)
        else:
            sdk_bat_file_path = os.path.join(pdir,sdk_bat_file)
            if os.path.exists(sdk_bat_file_path):
                debug('vc.py:find_batch_file() sdk_bat_file_path:%s'%sdk_bat_file_path)
                return (batfilename,sdk_bat_file_path)
    return (batfilename,None)


__INSTALLED_VCS_RUN = None
_VC_TOOLS_VERSION_FILE_PATH = ['Auxiliary', 'Build', 'Microsoft.VCToolsVersion.default.txt']
_VC_TOOLS_VERSION_FILE = os.sep.join(_VC_TOOLS_VERSION_FILE_PATH)

def _check_cl_exists_in_vc_dir(env, vc_dir, msvc_version):
    """Find the cl.exe on the filesystem in the vc_dir depending on
    TARGET_ARCH, HOST_ARCH and the msvc version. TARGET_ARCH and
    HOST_ARCH can be extracted from the passed env, unless its None,
    which then the native platform is assumed the host and target.

    Args:
        env: Environment
            a construction environment, usually if this is passed its
            because there is a desired TARGET_ARCH to be used when searching
            for a cl.exe
        vc_dir: str
            the path to the VC dir in the MSVC installation
        msvc_version: str
            msvc version (major.minor, e.g. 10.0)

    Returns:
        bool:

    """

    # determine if there is a specific target platform we want to build for and
    # use that to find a list of valid VCs, default is host platform == target platform
    # and same for if no env is specified to extract target platform from
    if env:
        (host_platform, target_platform, req_target_platform) = get_host_target(env)
    else:
        host_platform = platform.machine().lower()
        target_platform = host_platform

    host_platform = _ARCH_TO_CANONICAL[host_platform]
    target_platform = _ARCH_TO_CANONICAL[target_platform]

    debug('_check_cl_exists_in_vc_dir(): host platform %s, target platform %s for version %s' % (host_platform, target_platform, msvc_version))

    ver_num = float(get_msvc_version_numeric(msvc_version))

    # make sure the cl.exe exists meaning the tool is installed
    if ver_num > 14:
        # 2017 and newer allowed multiple versions of the VC toolset to be installed at the same time.
        # Just get the default tool version for now
        #TODO: support setting a specific minor VC version
        default_toolset_file = os.path.join(vc_dir, _VC_TOOLS_VERSION_FILE)
        try:
            with open(default_toolset_file) as f:
                vc_specific_version = f.readlines()[0].strip()
        except IOError:
            debug('_check_cl_exists_in_vc_dir(): failed to read ' + default_toolset_file)
            return False
        except IndexError:
            debug('_check_cl_exists_in_vc_dir(): failed to find MSVC version in ' + default_toolset_file)
            return False

        host_trgt_dir = _HOST_TARGET_TO_CL_DIR_GREATER_THAN_14.get((host_platform, target_platform), None)
        if host_trgt_dir is None:
            debug('_check_cl_exists_in_vc_dir(): unsupported host/target platform combo: (%s,%s)'%(host_platform, target_platform))
            return False

        cl_path = os.path.join(vc_dir, 'Tools','MSVC', vc_specific_version, 'bin',  host_trgt_dir[0], host_trgt_dir[1], _CL_EXE_NAME)
        debug('_check_cl_exists_in_vc_dir(): checking for ' + _CL_EXE_NAME + ' at ' + cl_path)
        if os.path.exists(cl_path):
            debug('_check_cl_exists_in_vc_dir(): found ' + _CL_EXE_NAME + '!')
            return True

    elif ver_num <= 14 and ver_num >= 8:

        # Set default value to be -1 as "" which is the value for x86/x86 yields true when tested
        # if not host_trgt_dir
        host_trgt_dir = _HOST_TARGET_TO_CL_DIR.get((host_platform, target_platform), None)
        if host_trgt_dir is None:
            debug('_check_cl_exists_in_vc_dir(): unsupported host/target platform combo')
            return False

        cl_path = os.path.join(vc_dir, 'bin',  host_trgt_dir, _CL_EXE_NAME)
        debug('_check_cl_exists_in_vc_dir(): checking for ' + _CL_EXE_NAME + ' at ' + cl_path)

        cl_path_exists = os.path.exists(cl_path)
        if not cl_path_exists and host_platform == 'amd64':
            # older versions of visual studio only had x86 binaries,
            # so if the host platform is amd64, we need to check cross
            # compile options (x86 binary compiles some other target on a 64 bit os)

            # Set default value to be -1 as "" which is the value for x86/x86 yields true when tested
            # if not host_trgt_dir
            host_trgt_dir = _HOST_TARGET_TO_CL_DIR.get(('x86', target_platform), None)
            if host_trgt_dir is None:
                return False

            cl_path = os.path.join(vc_dir, 'bin', host_trgt_dir, _CL_EXE_NAME)
            debug('_check_cl_exists_in_vc_dir(): checking for ' + _CL_EXE_NAME + ' at ' + cl_path)
            cl_path_exists = os.path.exists(cl_path)

        if cl_path_exists:
            debug('_check_cl_exists_in_vc_dir(): found ' + _CL_EXE_NAME + '!')
            return True

    elif ver_num < 8 and ver_num >= 6:
        # not sure about these versions so if a walk the VC dir (could be slow)
        for root, _, files in os.walk(vc_dir):
            if _CL_EXE_NAME in files:
                debug('get_installed_vcs ' + _CL_EXE_NAME + ' found %s' % os.path.join(root, _CL_EXE_NAME))
                return True
        return False
    else:
        # version not support return false
        debug('_check_cl_exists_in_vc_dir(): unsupported MSVC version: ' + str(ver_num))

    return False

def cached_get_installed_vcs(env=None):
    global __INSTALLED_VCS_RUN

    if __INSTALLED_VCS_RUN is None:
        ret = get_installed_vcs(env)
        __INSTALLED_VCS_RUN = ret

    return __INSTALLED_VCS_RUN

def get_installed_vcs(env=None):
    installed_versions = []

    for ver in _VCVER:
        debug('trying to find VC %s' % ver)
        try:
            VC_DIR = find_vc_pdir(ver)
            if VC_DIR:
                debug('found VC %s' % ver)
                if _check_cl_exists_in_vc_dir(env, VC_DIR, ver):
                    installed_versions.append(ver)
                else:
                    debug('find_vc_pdir no compiler found %s' % ver)
            else:
                debug('find_vc_pdir return None for ver %s' % ver)
        except (MSVCUnsupportedTargetArch, MSVCUnsupportedHostArch):
            # Allow this exception to propagate further as it should cause
            # SCons to exit with an error code
            raise
        except VisualCException as e:
            debug('did not find VC %s: caught exception %s' % (ver, str(e)))
    return installed_versions

def reset_installed_vcs():
    """Make it try again to find VC.  This is just for the tests."""
    __INSTALLED_VCS_RUN = None

# Running these batch files isn't cheap: most of the time spent in
# msvs.generate() is due to vcvars*.bat.  In a build that uses "tools='msvs'"
# in multiple environments, for example:
#    env1 = Environment(tools='msvs')
#    env2 = Environment(tools='msvs')
# we can greatly improve the speed of the second and subsequent Environment
# (or Clone) calls by memoizing the environment variables set by vcvars*.bat.
script_env_stdout_cache = {}
def script_env(script, args=None):
    cache_key = (script, args)
    stdout = script_env_stdout_cache.get(cache_key, None)
    if stdout is None:
        stdout = common.get_output(script, args)
        script_env_stdout_cache[cache_key] = stdout

    # Stupid batch files do not set return code: we take a look at the
    # beginning of the output for an error message instead
    olines = stdout.splitlines()
    if olines[0].startswith("The specified configuration type is missing"):
        raise BatchFileExecutionError("\n".join(olines[:2]))

    return common.parse_output(stdout)

def get_default_version(env):
    debug('get_default_version()')

    msvc_version = env.get('MSVC_VERSION')
    msvs_version = env.get('MSVS_VERSION')

    debug('get_default_version(): msvc_version:%s msvs_version:%s'%(msvc_version,msvs_version))

    if msvs_version and not msvc_version:
        SCons.Warnings.warn(
                SCons.Warnings.DeprecatedWarning,
                "MSVS_VERSION is deprecated: please use MSVC_VERSION instead ")
        return msvs_version
    elif msvc_version and msvs_version:
        if not msvc_version == msvs_version:
            SCons.Warnings.warn(
                    SCons.Warnings.VisualVersionMismatch,
                    "Requested msvc version (%s) and msvs version (%s) do " \
                    "not match: please use MSVC_VERSION only to request a " \
                    "visual studio version, MSVS_VERSION is deprecated" \
                    % (msvc_version, msvs_version))
        return msvs_version
    if not msvc_version:
        installed_vcs = cached_get_installed_vcs(env)
        debug('installed_vcs:%s' % installed_vcs)
        if not installed_vcs:
            #msg = 'No installed VCs'
            #debug('msv %s\n' % repr(msg))
            #SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, msg)
            debug('msvc_setup_env: No installed VCs')
            return None
        msvc_version = installed_vcs[0]
        debug('msvc_setup_env: using default installed MSVC version %s\n' % repr(msvc_version))

    return msvc_version

def msvc_setup_env_once(env):
    try:
        has_run  = env["MSVC_SETUP_RUN"]
    except KeyError:
        has_run = False

    if not has_run:
        msvc_setup_env(env)
        env["MSVC_SETUP_RUN"] = True

def msvc_find_valid_batch_script(env,version):
    debug('vc.py:msvc_find_valid_batch_script()')
    # Find the host platform, target platform, and if present the requested
    # target platform
    platforms = get_host_target(env)
    debug("vc.py: msvs_find_valid_batch_script(): host_platform %s, target_platform %s req_target_platform:%s" % platforms)

    host_platform, target_platform, req_target_platform = platforms
    try_target_archs = [target_platform]

    # VS2012 has a "cross compile" environment to build 64 bit
    # with x86_amd64 as the argument to the batch setup script
    if req_target_platform in ('amd64', 'x86_64'):
        try_target_archs.append('x86_amd64')
    elif not req_target_platform and target_platform in ['amd64', 'x86_64']:
        # There may not be "native" amd64, but maybe "cross" x86_amd64 tools
        try_target_archs.append('x86_amd64')
        # If the user hasn't specifically requested a TARGET_ARCH, and
        # The TARGET_ARCH is amd64 then also try 32 bits if there are no viable
        # 64 bit tools installed
        try_target_archs.append('x86')

    debug("msvs_find_valid_batch_script(): host_platform: %s try_target_archs:%s"%(host_platform, try_target_archs))

    d = None
    for tp in try_target_archs:
        # Set to current arch.
        env['TARGET_ARCH']=tp

        debug("vc.py:msvc_find_valid_batch_script() trying target_platform:%s"%tp)
        host_target = (host_platform, tp)
        if not is_host_target_supported(host_target, version):
            warn_msg = "host, target = %s not supported for MSVC version %s" % \
                (host_target, version)
            SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, warn_msg)
        arg = _HOST_TARGET_ARCH_TO_BAT_ARCH[host_target]

        # Get just version numbers
        maj, min = msvc_version_to_maj_min(version)
        # VS2015+
        if maj >= 14:
            if env.get('MSVC_UWP_APP') == '1':
                # Initialize environment variables with store/universal paths
                arg += ' store'

        # Try to locate a batch file for this host/target platform combo
        try:
            (vc_script,sdk_script) = find_batch_file(env,version,host_platform,tp)
            debug('vc.py:msvc_find_valid_batch_script() vc_script:%s sdk_script:%s'%(vc_script,sdk_script))
        except VisualCException as e:
            msg = str(e)
            debug('Caught exception while looking for batch file (%s)' % msg)
            warn_msg = "VC version %s not installed.  " + \
                       "C/C++ compilers are most likely not set correctly.\n" + \
                       " Installed versions are: %s"
            warn_msg = warn_msg % (version, cached_get_installed_vcs(env))
            SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, warn_msg)
            continue

        # Try to use the located batch file for this host/target platform combo
        debug('vc.py:msvc_find_valid_batch_script() use_script 2 %s, args:%s\n' % (repr(vc_script), arg))
        found = None
        if vc_script:
            try:
                d = script_env(vc_script, args=arg)
                found = vc_script
            except BatchFileExecutionError as e:
                debug('vc.py:msvc_find_valid_batch_script() use_script 3: failed running VC script %s: %s: Error:%s'%(repr(vc_script),arg,e))
                vc_script=None
                continue
        if not vc_script and sdk_script:
            debug('vc.py:msvc_find_valid_batch_script() use_script 4: trying sdk script: %s'%(sdk_script))
            try:
                d = script_env(sdk_script)
                found = sdk_script
            except BatchFileExecutionError as e:
                debug('vc.py:msvc_find_valid_batch_script() use_script 5: failed running SDK script %s: Error:%s'%(repr(sdk_script),e))
                continue
        elif not vc_script and not sdk_script:
            debug('vc.py:msvc_find_valid_batch_script() use_script 6: Neither VC script nor SDK script found')
            continue

        debug("vc.py:msvc_find_valid_batch_script() Found a working script/target: %s/%s"%(repr(found),arg))
        break # We've found a working target_platform, so stop looking

    # If we cannot find a viable installed compiler, reset the TARGET_ARCH
    # To it's initial value
    if not d:
        env['TARGET_ARCH']=req_target_platform

    return d


def msvc_setup_env(env):
    debug('msvc_setup_env()')

    version = get_default_version(env)
    if version is None:
        warn_msg = "No version of Visual Studio compiler found - C/C++ " \
                   "compilers most likely not set correctly"
        SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, warn_msg)
        return None
    debug('msvc_setup_env: using specified MSVC version %s\n' % repr(version))

    # XXX: we set-up both MSVS version for backward
    # compatibility with the msvs tool
    env['MSVC_VERSION'] = version
    env['MSVS_VERSION'] = version
    env['MSVS'] = {}


    use_script = env.get('MSVC_USE_SCRIPT', True)
    if SCons.Util.is_String(use_script):
        debug('vc.py:msvc_setup_env() use_script 1 %s\n' % repr(use_script))
        d = script_env(use_script)
    elif use_script:
        d = msvc_find_valid_batch_script(env,version)
        debug('vc.py:msvc_setup_env() use_script 2 %s\n' % d)
        if not d:
            return d
    else:
        debug('MSVC_USE_SCRIPT set to False')
        warn_msg = "MSVC_USE_SCRIPT set to False, assuming environment " \
                   "set correctly."
        SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, warn_msg)
        return None

    for k, v in d.items():
        debug('vc.py:msvc_setup_env() env:%s -> %s'%(k,v))
        env.PrependENVPath(k, v, delete_existing=True)

    # final check to issue a warning if the compiler is not present
    msvc_cl = find_program_path(env, 'cl')
    if not msvc_cl:
        SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning,
            "Could not find MSVC compiler 'cl', it may need to be installed separately with Visual Studio")

def msvc_exists(env=None, version=None):
    vcs = cached_get_installed_vcs(env)
    if version is None:
        return len(vcs) > 0
    return version in vcs
