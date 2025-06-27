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

"""
MS Compilers: Visual C/C++ detection and configuration.

# TODO:
#   * supported arch for versions: for old versions of batch file without
#     argument, giving bogus argument cannot be detected, so we have to hardcode
#     this here
#   * print warning when msvc version specified but not found
#   * find out why warning do not print
#   * test on 64 bits XP +  VS 2005 (and VS 6 if possible)
#   * SDK
#   * Assembly
"""

import SCons.compat

import subprocess
import os
import platform
import sysconfig
from pathlib import Path
from string import digits as string_digits
from subprocess import PIPE
import re
from collections import (
    namedtuple,
    OrderedDict,
)
import json
from functools import cmp_to_key
import enum

import SCons.Util
import SCons.Warnings
from SCons.Tool import find_program_path

from . import common
from .common import (
    CONFIG_CACHE,
    debug,
)
from .sdk import get_installed_sdks

from . import MSVC

from .MSVC.Exceptions import (
    VisualCException,
    MSVCInternalError,
    MSVCUserError,
    MSVCArgumentError,
    MSVCToolsetVersionNotFound,
)

# external exceptions

class MSVCUnsupportedHostArch(VisualCException):
    pass

class MSVCUnsupportedTargetArch(VisualCException):
    pass

class MSVCScriptNotFound(MSVCUserError):
    pass

class MSVCUseScriptError(MSVCUserError):
    pass

class MSVCUseSettingsError(MSVCUserError):
    pass

class VSWhereUserError(MSVCUserError):
    pass

# internal exceptions

class UnsupportedVersion(VisualCException):
    pass

class BatchFileExecutionError(VisualCException):
    pass

# undefined object for dict.get() in case key exists and value is None
UNDEFINED = object()

# powershell error sending telemetry for arm32 process on arm64 host (VS2019+):
#    True:  force VSCMD_SKIP_SENDTELEMETRY=1 (if necessary)
#    False: do nothing
_ARM32_ON_ARM64_SKIP_SENDTELEMETRY = True

# MSVC 9.0 preferred query order:
#     True:  VCForPython, VisualStudio
#     False: VisualStudio, VCForPython
_VC90_Prefer_VCForPython = False

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

# The msvc batch files report errors via stdout.  The following
# regular expression attempts to match known msvc error messages
# written to stdout.
re_script_output_error = re.compile(
    r'^(' + r'|'.join([
        r'VSINSTALLDIR variable is not set',             # 2002-2003
        r'The specified configuration type is missing',  # 2005+
        r'Error in script usage',                        # 2005+
        r'ERROR\:',                                      # 2005+
        r'\!ERROR\!',                                    # 2015-2015
        r'\[ERROR\:',                                    # 2017+
        r'\[ERROR\]',                                    # 2017+
        r'Syntax\:',                                     # 2017+
    ]) + r')'
)

# Lists of compatible host/target combinations are derived from a set of defined
# constant data structures for each host architecture. The derived data structures
# implicitly handle the differences in full versions and express versions of visual
# studio. The host/target combination search lists are contructed in order of
# preference. The construction of the derived data structures is independent of actual
# visual studio installations.  The host/target configurations are used in both the
# initial msvc detection and when finding a valid batch file for a given host/target
# combination.
#
# HostTargetConfig description:
#
#     label:
#         Name used for identification.
#
#     host_all_hosts:
#         Defined list of compatible architectures for each host architecture.
#
#     host_all_targets:
#         Defined list of target architectures for each host architecture.
#
#     host_def_targets:
#         Defined list of default target architectures for each host architecture.
#
#     all_pairs:
#         Derived list of all host/target combination tuples.
#
#     host_target_map:
#         Derived list of all compatible host/target combinations for each
#         supported host/target combination.
#
#     host_all_targets_map:
#         Derived list of all compatible host/target combinations for each
#         supported host.  This is used in the initial check that cl.exe exists
#         in the requisite visual studio vc host/target directory for a given host.
#
#     host_def_targets_map:
#         Derived list of default compatible host/target combinations for each
#         supported host.  This is used for a given host when the user does not
#         request a target archicture.
#
#     target_host_map:
#         Derived list of compatible host/target combinations for each supported
#         target/host combination.  This is used for a given host and target when
#         the user requests a target architecture.

_HOST_TARGET_CONFIG_NT = namedtuple("HostTargetConfig", [
    # defined
    "label",                # name for debugging/output
    "host_all_hosts",       # host_all_hosts[host] -> host_list
    "host_all_targets",     # host_all_targets[host] -> target_list
    "host_def_targets",     # host_def_targets[host] -> target_list
    # derived
    "all_pairs",            # host_target_list
    "host_target_map",      # host_target_map[host][target] -> host_target_list
    "host_all_targets_map", # host_all_targets_map[host][target] -> host_target_list
    "host_def_targets_map", # host_def_targets_map[host][target] -> host_target_list
    "target_host_map",      # target_host_map[target][host] -> host_target_list
])

def _host_target_config_factory(*, label, host_all_hosts, host_all_targets, host_def_targets):

    def _make_host_target_map(all_hosts, all_targets):
        # host_target_map[host][target] -> host_target_list
        host_target_map = {}
        for host, host_list in all_hosts.items():
            host_target_map[host] = {}
            for host_platform in host_list:
                for target_platform in all_targets[host_platform]:
                    if target_platform not in host_target_map[host]:
                        host_target_map[host][target_platform] = []
                    host_target_map[host][target_platform].append((host_platform, target_platform))
        return host_target_map

    def _make_host_all_targets_map(all_hosts, host_target_map, all_targets):
        # host_all_target_map[host] -> host_target_list
        # special host key '_all_' contains all (host,target) combinations
        all = '_all_'
        host_all_targets_map = {}
        host_all_targets_map[all] = []
        for host, host_list in all_hosts.items():
            host_all_targets_map[host] = []
            for host_platform in host_list:
                # all_targets[host_platform]: all targets for compatible host
                for target in all_targets[host_platform]:
                    for host_target in host_target_map[host_platform][target]:
                        for host_key in (host, all):
                            if host_target not in host_all_targets_map[host_key]:
                                host_all_targets_map[host_key].append(host_target)
        return host_all_targets_map

    def _make_host_def_targets_map(all_hosts, host_target_map, def_targets):
        # host_def_targets_map[host] -> host_target_list
        host_def_targets_map = {}
        for host, host_list in all_hosts.items():
            host_def_targets_map[host] = []
            for host_platform in host_list:
                # def_targets[host]: default targets for true host
                for target in def_targets[host]:
                    for host_target in host_target_map[host_platform][target]:
                        if host_target not in host_def_targets_map[host]:
                            host_def_targets_map[host].append(host_target)
        return host_def_targets_map

    def _make_target_host_map(all_hosts, host_all_targets_map):
        # target_host_map[target][host] -> host_target_list
        target_host_map = {}
        for host_platform in all_hosts.keys():
            for host_target in host_all_targets_map[host_platform]:
                _, target = host_target
                if target not in target_host_map:
                    target_host_map[target] = {}
                if host_platform not in target_host_map[target]:
                    target_host_map[target][host_platform] = []
                if host_target not in target_host_map[target][host_platform]:
                    target_host_map[target][host_platform].append(host_target)
        return target_host_map

    host_target_map = _make_host_target_map(host_all_hosts, host_all_targets)
    host_all_targets_map = _make_host_all_targets_map(host_all_hosts, host_target_map, host_all_targets)
    host_def_targets_map = _make_host_def_targets_map(host_all_hosts, host_target_map, host_def_targets)
    target_host_map = _make_target_host_map(host_all_hosts, host_all_targets_map)

    all_pairs = host_all_targets_map['_all_']
    del host_all_targets_map['_all_']

    host_target_cfg = _HOST_TARGET_CONFIG_NT(
        label = label,
        host_all_hosts = dict(host_all_hosts),
        host_all_targets = host_all_targets,
        host_def_targets = host_def_targets,
        all_pairs = all_pairs,
        host_target_map = host_target_map,
        host_all_targets_map = host_all_targets_map,
        host_def_targets_map = host_def_targets_map,
        target_host_map = target_host_map,
    )

    return host_target_cfg

# 14.1 (VS2017) and later

# Given a (host, target) tuple, return a tuple containing the batch file to
# look for and a tuple of path components to find cl.exe. We can't rely on returning
# an arg to use for vcvarsall.bat, because that script will run even if given
# a host/target pair that isn't installed.
#
# Starting with 14.1 (VS2017), the batch files are located in directory
# <VSROOT>/VC/Auxiliary/Build.  The batch file name is the first value of the
# stored tuple.
#
# The build tools are organized by host and target subdirectories under each toolset
# version directory.  For example,  <VSROOT>/VC/Tools/MSVC/14.31.31103/bin/Hostx64/x64.
# The cl path fragment under the toolset version folder is the second value of
# the stored tuple.

# 14.3 (VS2022) and later

_GE2022_HOST_TARGET_BATCHFILE_CLPATHCOMPS = {

    ('amd64', 'amd64') : ('vcvars64.bat',          ('bin', 'Hostx64', 'x64')),
    ('amd64', 'x86')   : ('vcvarsamd64_x86.bat',   ('bin', 'Hostx64', 'x86')),
    ('amd64', 'arm')   : ('vcvarsamd64_arm.bat',   ('bin', 'Hostx64', 'arm')),
    ('amd64', 'arm64') : ('vcvarsamd64_arm64.bat', ('bin', 'Hostx64', 'arm64')),

    ('x86',   'amd64') : ('vcvarsx86_amd64.bat',   ('bin', 'Hostx86', 'x64')),
    ('x86',   'x86')   : ('vcvars32.bat',          ('bin', 'Hostx86', 'x86')),
    ('x86',   'arm')   : ('vcvarsx86_arm.bat',     ('bin', 'Hostx86', 'arm')),
    ('x86',   'arm64') : ('vcvarsx86_arm64.bat',   ('bin', 'Hostx86', 'arm64')),

    ('arm64', 'amd64') : ('vcvarsarm64_amd64.bat', ('bin', 'Hostarm64', 'arm64_amd64')),
    ('arm64', 'x86')   : ('vcvarsarm64_x86.bat',   ('bin', 'Hostarm64', 'arm64_x86')),
    ('arm64', 'arm')   : ('vcvarsarm64_arm.bat',   ('bin', 'Hostarm64', 'arm64_arm')),
    ('arm64', 'arm64') : ('vcvarsarm64.bat',       ('bin', 'Hostarm64', 'arm64')),

}

_GE2022_HOST_TARGET_CFG = _host_target_config_factory(

    label = 'GE2022',

    host_all_hosts = OrderedDict([
        ('amd64', ['amd64', 'x86']),
        ('x86',   ['x86']),
        ('arm64', ['arm64', 'amd64', 'x86']),
        ('arm',   ['x86']),
    ]),

    host_all_targets = {
        'amd64': ['amd64', 'x86', 'arm64', 'arm'],
        'x86':   ['x86', 'amd64', 'arm', 'arm64'],
        'arm64': ['arm64', 'amd64', 'arm', 'x86'],
        'arm':   [],
    },

    host_def_targets = {
        'amd64': ['amd64', 'x86'],
        'x86':   ['x86'],
        'arm64': ['arm64', 'amd64', 'arm', 'x86'],
        'arm':   ['arm'],
    },

)

# debug("_GE2022_HOST_TARGET_CFG: %s", _GE2022_HOST_TARGET_CFG)

# 14.2 (VS2019) to 14.1 (VS2017)

_LE2019_HOST_TARGET_BATCHFILE_CLPATHCOMPS = {

    ('amd64', 'amd64') : ('vcvars64.bat',          ('bin', 'Hostx64', 'x64')),
    ('amd64', 'x86')   : ('vcvarsamd64_x86.bat',   ('bin', 'Hostx64', 'x86')),
    ('amd64', 'arm')   : ('vcvarsamd64_arm.bat',   ('bin', 'Hostx64', 'arm')),
    ('amd64', 'arm64') : ('vcvarsamd64_arm64.bat', ('bin', 'Hostx64', 'arm64')),

    ('x86',   'amd64') : ('vcvarsx86_amd64.bat',   ('bin', 'Hostx86', 'x64')),
    ('x86',   'x86')   : ('vcvars32.bat',          ('bin', 'Hostx86', 'x86')),
    ('x86',   'arm')   : ('vcvarsx86_arm.bat',     ('bin', 'Hostx86', 'arm')),
    ('x86',   'arm64') : ('vcvarsx86_arm64.bat',   ('bin', 'Hostx86', 'arm64')),

    ('arm64', 'amd64') : ('vcvars64.bat',          ('bin', 'Hostx64', 'x64')),
    ('arm64', 'x86')   : ('vcvarsamd64_x86.bat',   ('bin', 'Hostx64', 'x86')),
    ('arm64', 'arm')   : ('vcvarsamd64_arm.bat',   ('bin', 'Hostx64', 'arm')),
    ('arm64', 'arm64') : ('vcvarsamd64_arm64.bat', ('bin', 'Hostx64', 'arm64')),

}

_LE2019_HOST_TARGET_CFG = _host_target_config_factory(

    label = 'LE2019',

    host_all_hosts = OrderedDict([
        ('amd64', ['amd64', 'x86']),
        ('x86',   ['x86']),
        ('arm64', ['amd64', 'x86']),
        ('arm',   ['x86']),
    ]),

    host_all_targets = {
        'amd64': ['amd64', 'x86', 'arm64', 'arm'],
        'x86':   ['x86', 'amd64', 'arm', 'arm64'],
        'arm64': ['arm64', 'amd64', 'arm', 'x86'],
        'arm':   [],
    },

    host_def_targets = {
        'amd64': ['amd64', 'x86'],
        'x86':   ['x86'],
        'arm64': ['arm64', 'amd64', 'arm', 'x86'],
        'arm':   ['arm'],
    },

)

# debug("_LE2019_HOST_TARGET_CFG: %s", _LE2019_HOST_TARGET_CFG)

# 14.0 (VS2015) to 10.0 (VS2010)

# Given a (host, target) tuple, return a tuple containing the argument for
# the batch file and a tuple of the path components to find cl.exe.
#
# In 14.0 (VS2015) and earlier, the original x86 tools are in the tools
# bin directory (i.e., <VSROOT>/VC/bin).  Any other tools are in subdirectory
# named for the the host/target pair or a single name if the host==target.

_LE2015_HOST_TARGET_BATCHARG_BATCHFILE_CLPATHCOMPS = {

    ('amd64', 'amd64') : ('amd64',     'vcvars64.bat',         ('bin', 'amd64')),
    ('amd64', 'x86')   : ('amd64_x86', 'vcvarsamd64_x86.bat',  ('bin', 'amd64_x86')),
    ('amd64', 'arm')   : ('amd64_arm', 'vcvarsamd64_arm.bat',  ('bin', 'amd64_arm')),

    ('x86',   'amd64') : ('x86_amd64', 'vcvarsx86_amd64.bat',  ('bin', 'x86_amd64')),
    ('x86',   'x86')   : ('x86',       'vcvars32.bat',         ('bin', )),
    ('x86',   'arm')   : ('x86_arm',   'vcvarsx86_arm.bat',    ('bin', 'x86_arm')),
    ('x86',   'ia64')  : ('x86_ia64',  'vcvarsx86_ia64.bat',   ('bin', 'x86_ia64')),

    ('arm64', 'amd64') : ('amd64',     'vcvars64.bat',         ('bin', 'amd64')),
    ('arm64', 'x86')   : ('amd64_x86', 'vcvarsamd64_x86.bat',  ('bin', 'amd64_x86')),
    ('arm64', 'arm')   : ('amd64_arm', 'vcvarsamd64_arm.bat',  ('bin', 'amd64_arm')),

    ('arm',   'arm')   : ('arm',       'vcvarsarm.bat',        ('bin', 'arm')),
    ('ia64',  'ia64')  : ('ia64',      'vcvars64.bat',         ('bin', 'ia64')),

}

_LE2015_HOST_TARGET_CFG = _host_target_config_factory(

    label = 'LE2015',

    host_all_hosts = OrderedDict([
        ('amd64', ['amd64', 'x86']),
        ('x86',   ['x86']),
        ('arm64', ['amd64', 'x86']),
        ('arm',   ['arm']),
        ('ia64',  ['ia64']),
    ]),

    host_all_targets = {
        'amd64': ['amd64', 'x86', 'arm'],
        'x86':   ['x86', 'amd64', 'arm', 'ia64'],
        'arm64': ['amd64', 'x86', 'arm'],
        'arm':   ['arm'],
        'ia64':  ['ia64'],
    },

    host_def_targets = {
        'amd64': ['amd64', 'x86'],
        'x86':   ['x86'],
        'arm64': ['amd64', 'arm', 'x86'],
        'arm':   ['arm'],
        'ia64':  ['ia64'],
    },

)

# debug("_LE2015_HOST_TARGET_CFG: %s", _LE2015_HOST_TARGET_CFG)

# 9.0 (VS2008) to 8.0 (VS2005)

_LE2008_HOST_TARGET_BATCHARG_BATCHFILE_CLPATHCOMPS = {

    ('amd64', 'amd64') : ('amd64',     'vcvarsamd64.bat',      ('bin', 'amd64')),
    ('amd64', 'x86') :   ('x86',       'vcvars32.bat',         ('bin', )),

    ('x86',   'amd64') : ('x86_amd64', 'vcvarsx86_amd64.bat',  ('bin', 'x86_amd64')),
    ('x86',   'x86')   : ('x86',       'vcvars32.bat',         ('bin', )),
    ('x86',   'ia64')  : ('x86_ia64',  'vcvarsx86_ia64.bat',   ('bin', 'x86_ia64')),

    ('arm64', 'amd64') : ('amd64',     'vcvarsamd64.bat',      ('bin', 'amd64')),
    ('arm64', 'x86') :   ('x86',       'vcvars32.bat',         ('bin', )),

    ('ia64',  'ia64')  : ('ia64',      'vcvarsia64.bat',       ('bin', 'ia64')),

}

_LE2008_HOST_TARGET_CFG = _host_target_config_factory(

    label = 'LE2008',

    host_all_hosts = OrderedDict([
        ('amd64', ['amd64', 'x86']),
        ('x86',   ['x86']),
        ('arm64', ['amd64', 'x86']),
        ('ia64',  ['ia64']),
    ]),

    host_all_targets = {
        'amd64': ['amd64', 'x86'],
        'x86':   ['x86', 'amd64', 'ia64'],
        'arm64': ['amd64', 'x86'],
        'ia64':  ['ia64'],
    },

    host_def_targets = {
        'amd64': ['amd64', 'x86'],
        'x86':   ['x86'],
        'arm64': ['amd64', 'x86'],
        'ia64':  ['ia64'],
    },

)

# debug("_LE2008_HOST_TARGET_CFG: %s", _LE2008_HOST_TARGET_CFG)

# 7.1 (VS2003) and earlier

# For 7.1 (VS2003) and earlier, there are only x86 targets and the batch files
# take no arguments.

_LE2003_HOST_TARGET_CFG = _host_target_config_factory(

    label = 'LE2003',

    host_all_hosts = OrderedDict([
        ('amd64', ['x86']),
        ('x86',   ['x86']),
        ('arm64', ['x86']),
    ]),

    host_all_targets = {
        'amd64': ['x86'],
        'x86':   ['x86'],
        'arm64': ['x86'],
    },

    host_def_targets = {
        'amd64': ['x86'],
        'x86':   ['x86'],
        'arm64': ['x86'],
    },

)

# debug("_LE2003_HOST_TARGET_CFG: %s", _LE2003_HOST_TARGET_CFG)

_CL_EXE_NAME = 'cl.exe'

_VSWHERE_EXE = 'vswhere.exe'

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
    return ''.join([x for x in msvc_version if x in string_digits + '.'])

def get_host_platform(host_platform):

    host_platform = host_platform.lower()

    # Solaris returns i86pc for both 32 and 64 bit architectures
    if host_platform == 'i86pc':
        if platform.architecture()[0] == "64bit":
            host_platform = "amd64"
        else:
            host_platform = "x86"

    try:
        host =_ARCH_TO_CANONICAL[host_platform]
    except KeyError:
        msg = "Unrecognized host architecture %s"
        raise MSVCUnsupportedHostArch(msg % repr(host_platform)) from None

    return host

_native_host_architecture = None

def get_native_host_architecture():
    """Return the native host architecture."""
    global _native_host_architecture

    if _native_host_architecture is None:

        try:
            arch = common.read_reg(
                r'SYSTEM\CurrentControlSet\Control\Session Manager\Environment\PROCESSOR_ARCHITECTURE'
            )
        except OSError:
            arch = None

        if not arch:
            arch = platform.machine()

        _native_host_architecture = arch

    return _native_host_architecture

_native_host_platform = None

def get_native_host_platform():
    global _native_host_platform

    if _native_host_platform is None:
        arch = get_native_host_architecture()
        _native_host_platform = get_host_platform(arch)

    return _native_host_platform

def get_host_target(env, msvc_version, all_host_targets: bool=False):

    vernum = float(get_msvc_version_numeric(msvc_version))
    vernum_int = int(vernum * 10)

    if vernum_int >= 143:
        # 14.3 (VS2022) and later
        host_target_cfg = _GE2022_HOST_TARGET_CFG
    elif 143 > vernum_int >= 141:
        # 14.2 (VS2019) to 14.1 (VS2017)
        host_target_cfg = _LE2019_HOST_TARGET_CFG
    elif 141 > vernum_int >= 100:
        # 14.0 (VS2015) to 10.0 (VS2010)
        host_target_cfg = _LE2015_HOST_TARGET_CFG
    elif 100 > vernum_int >= 80:
        # 9.0 (VS2008) to 8.0 (VS2005)
        host_target_cfg = _LE2008_HOST_TARGET_CFG
    else: # 80 > vernum_int
        # 7.1 (VS2003) and earlier
        host_target_cfg = _LE2003_HOST_TARGET_CFG

    host_arch = env.get('HOST_ARCH') if env else None
    debug("HOST_ARCH:%s", str(host_arch))

    if host_arch:
        host_platform = get_host_platform(host_arch)
    else:
        host_platform = get_native_host_platform()

    target_arch = env.get('TARGET_ARCH') if env else None
    debug("TARGET_ARCH:%s", str(target_arch))

    if target_arch:

        try:
            target_platform = _ARCH_TO_CANONICAL[target_arch.lower()]
        except KeyError:
            all_archs = str(list(_ARCH_TO_CANONICAL.keys()))
            raise MSVCUnsupportedTargetArch(
                "Unrecognized target architecture %s\n\tValid architectures: %s"
                % (repr(target_arch), all_archs)
            ) from None

        target_host_map = host_target_cfg.target_host_map

        try:
            host_target_list = target_host_map[target_platform][host_platform]
        except KeyError:
            host_target_list = []
            warn_msg = "unsupported host, target combination ({}, {}) for MSVC version {}".format(
                repr(host_platform), repr(target_platform), msvc_version
            )
            SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, warn_msg)
            debug(warn_msg)

    else:

        target_platform = None

        if all_host_targets:
            host_targets_map = host_target_cfg.host_all_targets_map
        else:
            host_targets_map = host_target_cfg.host_def_targets_map

        try:
            host_target_list = host_targets_map[host_platform]
        except KeyError:
            msg = "Unrecognized host architecture %s for version %s"
            raise MSVCUnsupportedHostArch(msg % (repr(host_platform), msvc_version)) from None

    return host_platform, target_platform, host_target_list

_arm32_process_arm64_host = None

def is_arm32_process_arm64_host():
    global _arm32_process_arm64_host

    if _arm32_process_arm64_host is None:

        host = get_native_host_architecture()
        host = _ARCH_TO_CANONICAL.get(host.lower(),'')
        host_isarm64 = host == 'arm64'

        process = sysconfig.get_platform()
        process_isarm32 = process == 'win-arm32'

        _arm32_process_arm64_host = host_isarm64 and process_isarm32

    return _arm32_process_arm64_host

_check_skip_sendtelemetry = None

def _skip_sendtelemetry(env):
    global _check_skip_sendtelemetry

    if _check_skip_sendtelemetry is None:

        if _ARM32_ON_ARM64_SKIP_SENDTELEMETRY and is_arm32_process_arm64_host():
            _check_skip_sendtelemetry = True
        else:
            _check_skip_sendtelemetry = False

    if not _check_skip_sendtelemetry:
        return False

    msvc_version = env.get('MSVC_VERSION') if env else None
    if not msvc_version:
        msvc_version = msvc_default_version(env)

    if not msvc_version:
        return False

    vernum = float(get_msvc_version_numeric(msvc_version))
    if vernum < 14.2:  # VS2019
        return False

    # arm32 process, arm64 host, VS2019+
    return True

# If you update this, update SupportedVSList in Tool/MSCommon/vs.py, and the
# MSVC_VERSION documentation in Tool/msvc.xml.
_VCVER = [
    "14.3",
    "14.2",
    "14.1", "14.1Exp",
    "14.0", "14.0Exp",
    "12.0", "12.0Exp",
    "11.0", "11.0Exp",
    "10.0", "10.0Exp",
    "9.0", "9.0Exp",
    "8.0", "8.0Exp",
    "7.1",
    "7.0",
    "6.0"]

# VS2017 and later: use a single vswhere json query to find all installations

# vswhere query:
#    map vs major version to vc version (no suffix)
#    build set of supported vc versions (including suffix)

_VSWHERE_VSMAJOR_TO_VCVERSION = {}
_VSWHERE_SUPPORTED_VCVER = set()

for vs_major, vc_version, vc_ver_list in (
    ('17', '14.3', None),
    ('16', '14.2', None),
    ('15', '14.1', ['14.1Exp']),
):
    _VSWHERE_VSMAJOR_TO_VCVERSION[vs_major] = vc_version
    _VSWHERE_SUPPORTED_VCVER.add(vc_version)
    if vc_ver_list:
        for vc_ver in vc_ver_list:
            _VSWHERE_SUPPORTED_VCVER.add(vc_ver)

# vwhere query:
#    build of set of candidate component ids
#    preferred ranking: Enterprise, Professional, Community, BuildTools, Express
#      Ent, Pro, Com, BT, Exp are in the same list
#      Exp also has it's own list
#    currently, only the express (Exp) suffix is expected

_VSWHERE_COMPONENTID_CANDIDATES = set()
_VSWHERE_COMPONENTID_RANKING = {}
_VSWHERE_COMPONENTID_SUFFIX = {}
_VSWHERE_COMPONENTID_SCONS_SUFFIX = {}

for component_id, component_rank, component_suffix, scons_suffix in (
    ('Enterprise',   140, 'Ent', ''),
    ('Professional', 130, 'Pro', ''),
    ('Community',    120, 'Com', ''),
    ('BuildTools',   110, 'BT',  ''),
    ('WDExpress',    100, 'Exp', 'Exp'),
):
    _VSWHERE_COMPONENTID_CANDIDATES.add(component_id)
    _VSWHERE_COMPONENTID_RANKING[component_id] = component_rank
    _VSWHERE_COMPONENTID_SUFFIX[component_id] = component_suffix
    _VSWHERE_COMPONENTID_SCONS_SUFFIX[component_id] = scons_suffix

# VS2015 and earlier: configure registry queries to probe for installed VC editions
_VCVER_TO_PRODUCT_DIR = {
    '14.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\14.0\Setup\VC\ProductDir')
    ],
    '14.0Exp': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\WDExpress\14.0\Setup\VS\ProductDir'),
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\14.0\Setup\VC\ProductDir'),
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\14.0\Setup\VC\ProductDir')
    ],
    '12.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\12.0\Setup\VC\ProductDir'),
    ],
    '12.0Exp': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\WDExpress\12.0\Setup\VS\ProductDir'),
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\12.0\Setup\VC\ProductDir'),
    ],
    '11.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\11.0\Setup\VC\ProductDir'),
    ],
    '11.0Exp': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\WDExpress\11.0\Setup\VS\ProductDir'),
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\11.0\Setup\VC\ProductDir'),
    ],
    '10.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\10.0\Setup\VC\ProductDir'),
    ],
    '10.0Exp': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\10.0\Setup\VC\ProductDir'),
    ],
    '9.0': [
        (SCons.Util.HKEY_CURRENT_USER, r'Microsoft\DevDiv\VCForPython\9.0\installdir',),  # vs root
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\DevDiv\VCForPython\9.0\installdir',),  # vs root
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\9.0\Setup\VC\ProductDir',),
    ] if _VC90_Prefer_VCForPython else [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\9.0\Setup\VC\ProductDir',),
        (SCons.Util.HKEY_CURRENT_USER, r'Microsoft\DevDiv\VCForPython\9.0\installdir',),  # vs root
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\DevDiv\VCForPython\9.0\installdir',),  # vs root
    ],
    '9.0Exp': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\9.0\Setup\VC\ProductDir'),
    ],
    '8.0': [
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VisualStudio\8.0\Setup\VC\ProductDir'),
        (SCons.Util.HKEY_LOCAL_MACHINE, r'Microsoft\VCExpress\8.0\Setup\VC\ProductDir'),
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

# detect ide binaries

VS2022_VS2002_DEV = (
    MSVC.Kind.IDE_PROGRAM_DEVENV_COM,  # devenv.com
)

VS1998_DEV = (
    MSVC.Kind.IDE_PROGRAM_MSDEV_COM,  # MSDEV.COM
)

VS2017_EXP = (
    MSVC.Kind.IDE_PROGRAM_WDEXPRESS_EXE,  # WDExpress.exe
)

VS2015_VS2012_EXP = (
    MSVC.Kind.IDE_PROGRAM_WDEXPRESS_EXE,     # WDExpress.exe [Desktop]
    MSVC.Kind.IDE_PROGRAM_VSWINEXPRESS_EXE,  # VSWinExpress.exe [Windows]
    MSVC.Kind.IDE_PROGRAM_VWDEXPRESS_EXE,    # VWDExpress.exe [Web]
)

VS2010_VS2005_EXP = (
    MSVC.Kind.IDE_PROGRAM_VCEXPRESS_EXE,
)

# detect productdir kind

_DETECT = MSVC.Kind.VCVER_KIND_DETECT

_VCVER_KIND_DETECT = {

    #   'VCVer':   (relpath from pdir to vsroot, path from vsroot to ide binaries, ide binaries)

    '14.3':    _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV),  # 2022
    '14.2':    _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV),  # 2019
    '14.1':    _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2017_EXP),  # 2017
    '14.1Exp': _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2017_EXP),  # 2017

    '14.0':    _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2015_VS2012_EXP),  # 2015
    '14.0Exp': _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2015_VS2012_EXP),  # 2015
    '12.0':    _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2015_VS2012_EXP),  # 2013
    '12.0Exp': _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2015_VS2012_EXP),  # 2013
    '11.0':    _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2015_VS2012_EXP),  # 2012
    '11.0Exp': _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2015_VS2012_EXP),  # 2012

    '10.0':    _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2010_VS2005_EXP),  # 2010
    '10.0Exp': _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2010_VS2005_EXP),  # 2010
    '9.0':     _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2010_VS2005_EXP),  # 2008
    '9.0Exp':  _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2010_VS2005_EXP),  # 2008
    '8.0':     _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2010_VS2005_EXP),  # 2005
    '8.0Exp':  _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV + VS2010_VS2005_EXP),  # 2005

    '7.1':     _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV),  # 2003
    '7.0':     _DETECT(root='..', path=r'Common7\IDE', programs=VS2022_VS2002_DEV),  # 2001

    '6.0':     _DETECT(root='..', path=r'Common\MSDev98\Bin', programs=VS1998_DEV),  # 1998
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
    except ValueError:
        raise ValueError("Unrecognized version %s (%s)" % (msvc_version,msvc_version_numeric)) from None


_VSWHERE_EXEGROUP_MSVS = [os.path.join(p, _VSWHERE_EXE) for p in [
    # For bug 3333: support default location of vswhere for both
    # 64 and 32 bit windows installs.
    # For bug 3542: also accommodate not being on C: drive.
    os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"),
    os.path.expandvars(r"%ProgramFiles%\Microsoft Visual Studio\Installer"),
]]

_VSWHERE_EXEGROUP_PKGMGR = [os.path.join(p, _VSWHERE_EXE) for p in [
    os.path.expandvars(r"%ChocolateyInstall%\bin"),
    os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Links"),
    os.path.expanduser(r"~\scoop\shims"),
    os.path.expandvars(r"%SCOOP%\shims"),
]]

_VSWhereBinary = namedtuple('_VSWhereBinary', [
    'vswhere_exe',
    'vswhere_norm',
])

class VSWhereBinary(_VSWhereBinary, MSVC.Util.AutoInitialize):

    _UNDEFINED_VSWHERE_BINARY = None

    _cache_vswhere_paths = {}

    @classmethod
    def _initialize(cls):
        vswhere_binary = cls(
            vswhere_exe=None,
            vswhere_norm=None,
        )
        cls._UNDEFINED_VSWHERE_BINARY = vswhere_binary
        for symbol in (None, ''):
            cls._cache_vswhere_paths[symbol] = vswhere_binary

    @classmethod
    def factory(cls, vswhere_exe):

        if not vswhere_exe:
            return cls._UNDEFINED_VSWHERE_BINARY

        vswhere_binary = cls._cache_vswhere_paths.get(vswhere_exe)
        if vswhere_binary:
            return vswhere_binary

        vswhere_norm = MSVC.Util.normalize_path(vswhere_exe)

        vswhere_binary = cls._cache_vswhere_paths.get(vswhere_norm)
        if vswhere_binary:
            return vswhere_binary

        vswhere_binary = cls(
            vswhere_exe=vswhere_exe,
            vswhere_norm=vswhere_norm,
        )

        cls._cache_vswhere_paths[vswhere_exe] = vswhere_binary
        cls._cache_vswhere_paths[vswhere_norm] = vswhere_binary

        return vswhere_binary

class _VSWhereExecutable(MSVC.Util.AutoInitialize):

    debug_extra = None

    class _VSWhereUserPriority(enum.IntEnum):
        HIGH = 0               # before msvs locations
        DEFAULT = enum.auto()  # after msvs locations and before pkgmgr locations
        LOW = enum.auto()      # after pkgmgr locations

    priority_default = _VSWhereUserPriority.DEFAULT
    priority_map = {}
    priority_symbols = []
    priority_indmap = {}

    for e in _VSWhereUserPriority:
        for symbol in (e.name.lower(), e.name.lower().capitalize(), e.name):
            priority_map[symbol] = e.value
            priority_symbols.append(symbol)
        priority_indmap[e.value] = e.name

    UNDEFINED_VSWHERE_BINARY = VSWhereBinary.factory(None)

    @classmethod
    def reset(cls):

        cls._vswhere_exegroups = None
        cls._vswhere_exegroups_user = None
        cls._vswhere_exegroup_msvs = None
        cls._vswhere_exegroup_pkgmgr = None

        cls.vswhere_frozen_flag = False
        cls.vswhere_frozen_binary = None

    @classmethod
    def _initialize(cls):
        cls.debug_extra = common.debug_extra(cls)
        cls.reset()

    @classmethod
    def _vswhere_exegroup_binaries(cls, vswhere_exe_list, label):
        vswhere_binaries = []
        for vswhere_exe in vswhere_exe_list:
            if not os.path.exists(vswhere_exe):
                continue
            vswhere_binary = VSWhereBinary.factory(vswhere_exe)
            vswhere_binaries.append(vswhere_binary)
            debug(
                "insert exegroup=%s, vswhere_binary=%s",
                label, vswhere_binary, extra=cls.debug_extra
            )
        return vswhere_binaries

    @classmethod
    def _get_vswhere_exegroups(cls):

        if cls._vswhere_exegroups is None:

            cls._vswhere_exegroup_msvs = cls._vswhere_exegroup_binaries(_VSWHERE_EXEGROUP_MSVS, 'MSVS')
            cls._vswhere_exegroup_pkgmgr = cls._vswhere_exegroup_binaries(_VSWHERE_EXEGROUP_PKGMGR, 'PKGMGR')

            cls._vswhere_exegroups_user = [
                [] for e in cls._VSWhereUserPriority
            ]

            vswhere_exegroups = [
                cls._vswhere_exegroups_user[cls._VSWhereUserPriority.HIGH],
                cls._vswhere_exegroup_msvs,
                cls._vswhere_exegroups_user[cls._VSWhereUserPriority.DEFAULT],
                cls._vswhere_exegroup_pkgmgr,
                cls._vswhere_exegroups_user[cls._VSWhereUserPriority.LOW],
            ]

            cls._vswhere_exegroups = vswhere_exegroups

        return cls._vswhere_exegroups

    @classmethod
    def _get_vswhere_exegroups_user(cls):

        if cls._vswhere_exegroups_user is None:
            cls._get_vswhere_exegroups()

        return cls._vswhere_exegroups_user

    @classmethod
    def _vswhere_exegroup_binary(cls, exegroup):
        # first vswhere binary in group or UNDEFINED_VSWHERE_BINARY
        vswhere_binary = exegroup[0] if exegroup else cls.UNDEFINED_VSWHERE_BINARY
        return vswhere_binary

    @classmethod
    def _vswhere_current_binary(cls):
        # first vswhere binary in priority order or UNDEFINED_VSWHERE_BINARY
        vswhere_binary = cls.UNDEFINED_VSWHERE_BINARY
        vswhere_exegroups = cls._get_vswhere_exegroups()
        for exegroup in vswhere_exegroups:
            binary = cls._vswhere_exegroup_binary(exegroup)
            if binary == cls.UNDEFINED_VSWHERE_BINARY:
                continue
            vswhere_binary = binary
            break
        return vswhere_binary

    @classmethod
    def _vswhere_all_executables(cls):
        # unique vswhere executables in priority order
        vswhere_exe_list = []
        vswhere_norm_set = set()
        vswhere_exegroups = cls._get_vswhere_exegroups()
        for exegroup in vswhere_exegroups:
            for vswhere_binary in exegroup:
                if vswhere_binary.vswhere_norm in vswhere_norm_set:
                    continue
                vswhere_norm_set.add(vswhere_binary.vswhere_norm)
                vswhere_exe_list.append(vswhere_binary.vswhere_exe)
        return vswhere_exe_list

    @classmethod
    def is_frozen(cls) -> bool:
        rval = bool(cls.vswhere_frozen_flag)
        return rval

    @classmethod
    def freeze_vswhere_binary(cls):
        if not cls.vswhere_frozen_flag:
            cls.vswhere_frozen_flag = True
            cls.vswhere_frozen_binary = cls._vswhere_current_binary()
            debug("freeze=%s", cls.vswhere_frozen_binary, extra=cls.debug_extra)
        return cls.vswhere_frozen_binary

    @classmethod
    def freeze_vswhere_executable(cls):
        vswhere_binary = cls.freeze_vswhere_binary()
        vswhere_exe = vswhere_binary.vswhere_exe
        return vswhere_exe

    @classmethod
    def get_vswhere_executable(cls):
        if cls.vswhere_frozen_flag:
            vswhere_binary = cls.vswhere_frozen_binary
        else:
            vswhere_binary = cls._vswhere_current_binary()
        vswhere_exe = vswhere_binary.vswhere_exe
        debug("vswhere_exe=%s", repr(vswhere_exe), extra=cls.debug_extra)
        return vswhere_exe

    @classmethod
    def _vswhere_priority_group(cls, priority):
        if not priority:
            group = cls.priority_default
        else:
            group = cls.priority_map.get(priority)
            if group is None:
                msg = f'Value specified for vswhere executable priority is not supported: {priority!r}:\n' \
                      f'  Valid values are: {cls.priority_symbols}'
                debug(f'VSWhereUserError: {msg}', extra=cls.debug_extra)
                raise VSWhereUserError(msg)
        return group

    @classmethod
    def register_vswhere_executable(cls, vswhere_exe, priority=None):

        vswhere_binary = cls.UNDEFINED_VSWHERE_BINARY

        if not vswhere_exe:
            # ignore: None or empty
            return vswhere_binary

        if not os.path.exists(vswhere_exe):
            msg = f'Specified vswhere executable not found: {vswhere_exe!r}.'
            debug(f'VSWhereUserError: {msg}', extra=cls.debug_extra)
            raise VSWhereUserError(msg)

        group = cls._vswhere_priority_group(priority)

        vswhere_binary = VSWhereBinary.factory(vswhere_exe)

        if cls.vswhere_frozen_flag:

            if vswhere_binary.vswhere_norm == cls.vswhere_frozen_binary.vswhere_norm:
                # ignore: user executable == frozen executable
                return vswhere_binary

            msg = 'A different vswhere execuable cannot be requested after initial detetection:\n' \
                  f'  initial vswhere executable:  {cls.vswhere_frozen_binary.vswhere_exe!r}\n' \
                  f'  request vswhere executable: {vswhere_binary.vswhere_exe!r}'

            debug(f'VSWhereUserError: {msg}', extra=cls.debug_extra)
            raise VSWhereUserError(msg)

        vswhere_exegroups_user = cls._get_vswhere_exegroups_user()

        exegroup = vswhere_exegroups_user[group]
        group_binary = cls._vswhere_exegroup_binary(exegroup)

        if vswhere_binary.vswhere_norm == group_binary.vswhere_norm:
            # ignore: user executable == exegroup[0] executable
            return vswhere_binary

        exegroup.insert(0, vswhere_binary)
        debug(
            "insert exegroup=user[%s], vswhere_binary=%s",
            cls.priority_indmap[group], vswhere_binary, extra=cls.debug_extra
        )

        return vswhere_binary

    @classmethod
    def vswhere_freeze_executable(cls, vswhere_exe):
        vswhere_binary = cls.register_vswhere_executable(vswhere_exe, priority='high')
        frozen_binary = cls.freeze_vswhere_binary()
        return frozen_binary, vswhere_binary

    @classmethod
    def vswhere_freeze_env(cls, env):

        if env is None:
            # no environment, VSWHERE undefined
            vswhere_exe = None
            write_vswhere = False
        elif not env.get('VSWHERE'):
            # environment, VSWHERE undefined/none/empty
            vswhere_exe = None
            write_vswhere = True
        else:
            # environment, VSWHERE defined
            vswhere_exe = env.subst('$VSWHERE')
            write_vswhere = False

        frozen_binary, vswhere_binary = cls.vswhere_freeze_executable(vswhere_exe)

        if write_vswhere and frozen_binary.vswhere_norm != vswhere_binary.vswhere_norm:
            env['VSWHERE'] = frozen_binary.vswhere_exe
            debug(
                "env['VSWHERE']=%s",
                repr(frozen_binary.vswhere_exe), extra=cls.debug_extra
            )

        return frozen_binary, vswhere_binary

# external use

def vswhere_register_executable(vswhere_exe, priority=None, freeze=False):
    debug(
        'register vswhere_exe=%s, priority=%s, freeze=%s',
        repr(vswhere_exe), repr(priority), repr(freeze)
    )
    _VSWhereExecutable.register_vswhere_executable(vswhere_exe, priority=priority)
    if freeze:
        _VSWhereExecutable.freeze_vswhere_executable()
    rval = _VSWhereExecutable.get_vswhere_executable()
    debug('current vswhere_exe=%s, is_frozen=%s', repr(rval), _VSWhereExecutable.is_frozen())
    return vswhere_exe

def vswhere_get_executable():
    debug('')
    vswhere_exe = _VSWhereExecutable.get_vswhere_executable()
    return vswhere_exe

def vswhere_freeze_executable():
    debug('')
    vswhere_exe = _VSWhereExecutable.freeze_vswhere_executable()
    return vswhere_exe

# internal use only
vswhere_freeze_env = _VSWhereExecutable.vswhere_freeze_env

def msvc_find_vswhere():
    """ Find the location of vswhere """
    # NB: this gets called from testsuite on non-Windows platforms.
    # Whether that makes sense or not, don't break it for those.
    vswhere_path = _VSWhereExecutable.get_vswhere_executable()
    return vswhere_path

_MSVCInstance = namedtuple('_MSVCInstance', [
    'vc_path',
    'vc_version',
    'vc_version_numeric',
    'vc_version_scons',
    'vc_release',
    'vc_component_id',
    'vc_component_rank',
    'vc_component_suffix',
])

class MSVCInstance(_MSVCInstance):

    @staticmethod
    def msvc_instances_default_order(a, b):
        # vc version numeric: descending order
        if a.vc_version_numeric != b.vc_version_numeric:
            return 1 if a.vc_version_numeric < b.vc_version_numeric else -1
        # vc release: descending order (release, preview)
        if a.vc_release != b.vc_release:
            return 1 if a.vc_release < b.vc_release else -1
        # component rank: descending order
        if a.vc_component_rank != b.vc_component_rank:
            return 1 if a.vc_component_rank < b.vc_component_rank else -1
        return 0

class _VSWhere(MSVC.Util.AutoInitialize):

    debug_extra = None

    @classmethod
    def reset(cls):

        cls.seen_vswhere = set()
        cls.seen_root = set()

        cls.msvc_instances = []
        cls.msvc_map = {}

    @classmethod
    def _initialize(cls):
        cls.debug_extra = common.debug_extra(cls)
        cls.reset()

    @classmethod
    def _filter_vswhere_binary(cls, vswhere_binary):

        vswhere_norm = None

        if vswhere_binary.vswhere_norm not in cls.seen_vswhere:
            cls.seen_vswhere.add(vswhere_binary.vswhere_norm)
            vswhere_norm = vswhere_binary.vswhere_norm

        return vswhere_norm

    @classmethod
    def _new_roots_discovered(cls):
        if len(cls.seen_vswhere) > 1:
            raise MSVCInternalError(f'vswhere discovered new msvc installations after initial detection')

    @classmethod
    def _vswhere_query_json_output(cls, vswhere_exe, vswhere_args):

        vswhere_json = None

        once = True
        while once:
            once = False
            # using break for single exit (unless exception)

            vswhere_cmd = [vswhere_exe] + vswhere_args + ['-format', 'json', '-utf8']
            debug("running: %s", vswhere_cmd, extra=cls.debug_extra)

            try:
                cp = subprocess.run(vswhere_cmd, stdout=PIPE, stderr=PIPE, check=True)
            except OSError as e:
                errmsg = str(e)
                debug("%s: %s", type(e).__name__, errmsg, extra=cls.debug_extra)
                break
            except Exception as e:
                errmsg = str(e)
                debug("%s: %s", type(e).__name__, errmsg, extra=cls.debug_extra)
                raise

            if not cp.stdout:
                debug("no vswhere information returned", extra=cls.debug_extra)
                break

            vswhere_output = cp.stdout.decode('utf8', errors='replace')
            if not vswhere_output:
                debug("no vswhere information output", extra=cls.debug_extra)
                break

            try:
                vswhere_output_json = json.loads(vswhere_output)
            except json.decoder.JSONDecodeError:
                debug("json decode exception loading vswhere output", extra=cls.debug_extra)
                break

            vswhere_json = vswhere_output_json
            break

        debug(
            'vswhere_json=%s, vswhere_exe=%s',
            bool(vswhere_json), repr(vswhere_exe), extra=cls.debug_extra
        )

        return vswhere_json

    @classmethod
    def vswhere_update_msvc_map(cls, vswhere_exe):

        frozen_binary, vswhere_binary = _VSWhereExecutable.vswhere_freeze_executable(vswhere_exe)

        vswhere_norm = cls._filter_vswhere_binary(frozen_binary)
        if not vswhere_norm:
            return cls.msvc_map

        debug('vswhere_norm=%s', repr(vswhere_norm), extra=cls.debug_extra)

        vswhere_json = cls._vswhere_query_json_output(
            vswhere_norm,
            ['-all', '-products', '*']
        )

        if not vswhere_json:
            return cls.msvc_map

        n_instances = len(cls.msvc_instances)

        for instance in vswhere_json:

            #print(json.dumps(instance, indent=4, sort_keys=True))

            installation_path = instance.get('installationPath')
            if not installation_path or not os.path.exists(installation_path):
                continue

            vc_path = os.path.join(installation_path, 'VC')
            if not os.path.exists(vc_path):
                continue

            vc_root = MSVC.Util.normalize_path(vc_path)
            if vc_root in cls.seen_root:
                continue
            cls.seen_root.add(vc_root)

            installation_version = instance.get('installationVersion')
            if not installation_version:
                continue

            vs_major = installation_version.split('.')[0]
            if not vs_major in _VSWHERE_VSMAJOR_TO_VCVERSION:
                debug('ignore vs_major: %s', vs_major, extra=cls.debug_extra)
                continue

            vc_version = _VSWHERE_VSMAJOR_TO_VCVERSION[vs_major]

            product_id = instance.get('productId')
            if not product_id:
                continue

            component_id = product_id.split('.')[-1]
            if component_id not in _VSWHERE_COMPONENTID_CANDIDATES:
                debug('ignore component_id: %s', component_id, extra=cls.debug_extra)
                continue

            component_rank = _VSWHERE_COMPONENTID_RANKING.get(component_id,0)
            if component_rank == 0:
                raise MSVCInternalError(f'unknown component_rank for component_id: {component_id!r}')

            scons_suffix = _VSWHERE_COMPONENTID_SCONS_SUFFIX[component_id]

            if scons_suffix:
                vc_version_scons = vc_version + scons_suffix
            else:
                vc_version_scons = vc_version

            is_prerelease = True if instance.get('isPrerelease', False) else False
            is_release = False if is_prerelease else True

            msvc_instance = MSVCInstance(
                vc_path = vc_path,
                vc_version = vc_version,
                vc_version_numeric = float(vc_version),
                vc_version_scons = vc_version_scons,
                vc_release = is_release,
                vc_component_id = component_id,
                vc_component_rank = component_rank,
                vc_component_suffix = component_suffix,
            )

            cls.msvc_instances.append(msvc_instance)

        new_roots = bool(len(cls.msvc_instances) > n_instances)
        if new_roots:

            cls.msvc_instances = sorted(
                cls.msvc_instances,
                key=cmp_to_key(MSVCInstance.msvc_instances_default_order)
            )

            cls.msvc_map = {}

            for msvc_instance in cls.msvc_instances:

                debug(
                    'msvc instance: msvc_version=%s, is_release=%s, component_id=%s, vc_path=%s',
                    repr(msvc_instance.vc_version_scons), msvc_instance.vc_release,
                    repr(msvc_instance.vc_component_id), repr(msvc_instance.vc_path),
                    extra=cls.debug_extra
                )

                key = (msvc_instance.vc_version_scons, msvc_instance.vc_release)
                cls.msvc_map.setdefault(key,[]).append(msvc_instance)

                if msvc_instance.vc_version_scons == msvc_instance.vc_version:
                    continue

                key = (msvc_instance.vc_version, msvc_instance.vc_release)
                cls.msvc_map.setdefault(key,[]).append(msvc_instance)

            cls._new_roots_discovered()

        debug(
            'new_roots=%s, msvc_instances=%s',
            new_roots, len(cls.msvc_instances), extra=cls.debug_extra
        )

        return cls.msvc_map

_cache_pdir_vswhere_queries = {}

def _find_vc_pdir_vswhere(msvc_version, vswhere_exe):
    """ Find the MSVC product directory using the vswhere program.

    Args:
        msvc_version: MSVC version to search for
        vswhere_exe: vswhere executable or None

    Returns:
        MSVC install dir or None

    Raises:
        UnsupportedVersion: if the version is not known by this file

    """
    global _cache_pdir_vswhere_queries

    msvc_map = _VSWhere.vswhere_update_msvc_map(vswhere_exe)
    if not msvc_map:
        return None

    rval = _cache_pdir_vswhere_queries.get(msvc_version, UNDEFINED)
    if rval != UNDEFINED:
        debug('msvc_version=%s, pdir=%s', repr(msvc_version), repr(rval))
        return rval

    if msvc_version not in _VSWHERE_SUPPORTED_VCVER:
        debug("Unknown version of MSVC: %s", msvc_version)
        raise UnsupportedVersion("Unknown version %s" % msvc_version)

    is_release = True
    key = (msvc_version, is_release)

    msvc_instances = msvc_map.get(key, UNDEFINED)
    if msvc_instances == UNDEFINED:
        debug(
            'msvc instances lookup failed: msvc_version=%s, is_release=%s',
            repr(msvc_version), repr(is_release)
        )
        msvc_instances = []

    save_pdir_kind = []

    pdir = None
    kind_t = None

    for msvc_instance in msvc_instances:

        vcdir = msvc_instance.vc_path

        vckind_t = MSVC.Kind.msvc_version_pdir_vswhere_kind(msvc_version, vcdir, _VCVER_KIND_DETECT[msvc_version])
        if vckind_t.skip:
            if vckind_t.save:
                debug('save kind: msvc_version=%s, pdir=%s', repr(msvc_version), repr(vcdir))
                save_pdir_kind.append((vcdir, vckind_t))
            else:
                debug('skip kind: msvc_version=%s, pdir=%s', repr(msvc_version), repr(vcdir))
            continue

        pdir = vcdir
        kind_t = vckind_t
        break

    if not pdir and not kind_t:
        if save_pdir_kind:
            pdir, kind_t = save_pdir_kind[0]

    MSVC.Kind.msvc_version_register_kind(msvc_version, kind_t)

    debug('msvc_version=%s, pdir=%s', repr(msvc_version), repr(pdir))
    _cache_pdir_vswhere_queries[msvc_version] = pdir

    return pdir

_cache_pdir_registry_queries = {}

def _find_vc_pdir_registry(msvc_version):
    """ Find the MSVC product directory using the registry.

    Args:
        msvc_version: MSVC version to search for

    Returns:
        MSVC install dir or None

    Raises:
        UnsupportedVersion: if the version is not known by this file

    """
    global _cache_pdir_registry_queries

    rval = _cache_pdir_registry_queries.get(msvc_version, UNDEFINED)
    if rval != UNDEFINED:
        debug('msvc_version=%s, pdir=%s', repr(msvc_version), repr(rval))
        return rval

    try:
        regkeys = _VCVER_TO_PRODUCT_DIR[msvc_version]
    except KeyError:
        debug("Unknown version of MSVC: %s", msvc_version)
        raise UnsupportedVersion("Unknown version %s" % msvc_version) from None

    save_pdir_kind = []

    is_win64 = common.is_win64()

    pdir = None
    kind_t = None

    root = 'Software\\'
    for hkroot, key in regkeys:

        if not hkroot or not key:
            continue

        if is_win64:
            msregkeys = [root + 'Wow6432Node\\' + key, root + key]
        else:
            msregkeys = [root + key]

        vcdir = None
        for msregkey in msregkeys:
            debug('trying VC registry key %s', repr(msregkey))
            try:
                vcdir = common.read_reg(msregkey, hkroot)
            except OSError:
                continue
            if vcdir:
                break

        if not vcdir:
            debug('no VC registry key %s', repr(key))
            continue

        is_vcforpython = False

        is_vsroot = False
        if msvc_version == '9.0' and key.lower().endswith('\\vcforpython\\9.0\\installdir'):
            # Visual C++ for Python registry key is VS installdir (root) not VC productdir
            is_vsroot = True
            is_vcforpython = True
        elif msvc_version in ('14.0Exp', '12.0Exp', '11.0Exp') and key.lower().endswith('\\setup\\vs\\productdir'):
            # Visual Studio 2015/2013/2012 Express is VS productdir (root) not VC productdir
            is_vsroot = True

        if is_vsroot:
            vcdir = os.path.join(vcdir, 'VC')
            debug('convert vs root to vc dir: %s', repr(vcdir))

        if not os.path.exists(vcdir):
            debug('reg says dir is %s, but it does not exist. (ignoring)', repr(vcdir))
            continue

        vckind_t = MSVC.Kind.msvc_version_pdir_registry_kind(msvc_version, vcdir, _VCVER_KIND_DETECT[msvc_version], is_vcforpython)
        if vckind_t.skip:
            if vckind_t.save:
                debug('save kind: msvc_version=%s, pdir=%s', repr(msvc_version), repr(vcdir))
                save_pdir_kind.append((vcdir, vckind_t))
            else:
                debug('skip kind: msvc_version=%s, pdir=%s', repr(msvc_version), repr(vcdir))
            continue

        pdir = vcdir
        kind_t = vckind_t
        break

    if not pdir and not kind_t:
        if save_pdir_kind:
            pdir, kind_t = save_pdir_kind[0]

    MSVC.Kind.msvc_version_register_kind(msvc_version, kind_t)

    debug('msvc_version=%s, pdir=%s', repr(msvc_version), repr(pdir))
    _cache_pdir_registry_queries[msvc_version] = pdir

    return pdir

def _find_vc_pdir(msvc_version, vswhere_exe):
    """Find the MSVC product directory for the given version.

    Args:
        msvc_version: str
            msvc version (major.minor, e.g. 10.0)

        vswhere_exe: str
            vswhere executable or None

    Returns:
        str: Path found in registry, or None

    Raises:
        UnsupportedVersion: if the version is not known by this file.

        UnsupportedVersion inherits from VisualCException.

    """

    if msvc_version in _VSWHERE_SUPPORTED_VCVER:

        pdir = _find_vc_pdir_vswhere(msvc_version, vswhere_exe)
        if pdir:
            debug('VC found: %s, dir=%s', repr(msvc_version), repr(pdir))
            return pdir

    elif msvc_version in _VCVER_TO_PRODUCT_DIR:

        pdir = _find_vc_pdir_registry(msvc_version)
        if pdir:
            debug('VC found: %s, dir=%s', repr(msvc_version), repr(pdir))
            return pdir

    else:

        debug("Unknown version of MSVC: %s", repr(msvc_version))
        raise UnsupportedVersion("Unknown version %s" % repr(msvc_version)) from None

    debug('no VC found for version %s', repr(msvc_version))
    return None

def find_vc_pdir(msvc_version, env=None):
    """Find the MSVC product directory for the given version.

    Args:
        msvc_version: str
            msvc version (major.minor, e.g. 10.0)
        env:
            optional to look up VSWHERE variable

    Returns:
        str: Path found in registry, or None

    Raises:
        UnsupportedVersion: if the version is not known by this file.

        UnsupportedVersion inherits from VisualCException.

    """

    frozen_binary, _ = _VSWhereExecutable.vswhere_freeze_env(env)

    pdir = _find_vc_pdir(msvc_version, frozen_binary.vswhere_exe)
    debug('pdir=%s', repr(pdir))

    return pdir

# register find_vc_pdir function with Kind routines
# in case of unknown msvc_version (just in case)
MSVC.Kind.register_msvc_version_pdir_func(find_vc_pdir)

def _reset_vc_pdir():
    debug('reset pdir caches')
    global _cache_pdir_vswhere_queries
    global _cache_pdir_registry_queries
    _cache_pdir_vswhere_queries = {}
    _cache_pdir_registry_queries = {}

def find_batch_file(msvc_version, host_arch, target_arch, pdir):
    """
    Find the location of the batch script which should set up the compiler
    for any TARGET_ARCH whose compilers were installed by Visual Studio/VCExpress

    In newer (2017+) compilers, make use of the fact there are vcvars
    scripts named with a host_target pair that calls vcvarsall.bat properly,
    so use that and return an empty argument.
    """

    # filter out e.g. "Exp" from the version name
    vernum = float(get_msvc_version_numeric(msvc_version))
    vernum_int = int(vernum * 10)

    sdk_pdir = pdir

    arg = ''
    vcdir = None
    clexe = None
    depbat = None

    if vernum_int >= 143:
        # 14.3 (VS2022) and later
        batfiledir = os.path.join(pdir, "Auxiliary", "Build")
        batfile, _ = _GE2022_HOST_TARGET_BATCHFILE_CLPATHCOMPS[(host_arch, target_arch)]
        batfilename = os.path.join(batfiledir, batfile)
        vcdir = pdir
    elif 143 > vernum_int >= 141:
        # 14.2 (VS2019) to 14.1 (VS2017)
        batfiledir = os.path.join(pdir, "Auxiliary", "Build")
        batfile, _ = _LE2019_HOST_TARGET_BATCHFILE_CLPATHCOMPS[(host_arch, target_arch)]
        batfilename = os.path.join(batfiledir, batfile)
        vcdir = pdir
    elif 141 > vernum_int >= 100:
        # 14.0 (VS2015) to 10.0 (VS2010)
        arg, batfile, cl_path_comps = _LE2015_HOST_TARGET_BATCHARG_BATCHFILE_CLPATHCOMPS[(host_arch, target_arch)]
        batfilename = os.path.join(pdir, "vcvarsall.bat")
        depbat = os.path.join(pdir, *cl_path_comps, batfile)
        clexe = os.path.join(pdir, *cl_path_comps, _CL_EXE_NAME)
    elif 100 > vernum_int >= 80:
        # 9.0 (VS2008) to 8.0 (VS2005)
        arg, batfile, cl_path_comps = _LE2008_HOST_TARGET_BATCHARG_BATCHFILE_CLPATHCOMPS[(host_arch, target_arch)]
        if vernum_int == 90 and MSVC.Kind.msvc_version_is_vcforpython(msvc_version):
            # 9.0 (VS2008) Visual C++ for Python:
            #     sdk batch files do not point to the VCForPython installation
            #     vcvarsall batch file is in installdir not productdir (e.g., vc\..\vcvarsall.bat)
            #     dependency batch files are not called from vcvarsall.bat
            sdk_pdir = None
            batfilename = os.path.join(pdir, os.pardir, "vcvarsall.bat")
            depbat = None
        else:
            batfilename = os.path.join(pdir, "vcvarsall.bat")
            depbat = os.path.join(pdir, *cl_path_comps, batfile)
        clexe = os.path.join(pdir, *cl_path_comps, _CL_EXE_NAME)
    else:  # 80 > vernum_int
        # 7.1 (VS2003) and earlier
        pdir = os.path.join(pdir, "Bin")
        batfilename = os.path.join(pdir, "vcvars32.bat")
        clexe = os.path.join(pdir, _CL_EXE_NAME)

    if not os.path.exists(batfilename):
        debug("batch file not found: %s", batfilename)
        batfilename = None

    if clexe and not os.path.exists(clexe):
        debug("%s not found: %s", _CL_EXE_NAME, clexe)
        batfilename = None

    if depbat and not os.path.exists(depbat):
        debug("dependency batch file not found: %s", depbat)
        batfilename = None

    return batfilename, arg, vcdir, sdk_pdir

def find_batch_file_sdk(host_arch, target_arch, sdk_pdir):
    """
    Find the location of the sdk batch script which should set up the compiler
    for any TARGET_ARCH whose compilers were installed by Visual Studio/VCExpress
    """

    installed_sdks = get_installed_sdks()
    for _sdk in installed_sdks:
        sdk_bat_file = _sdk.get_sdk_vc_script(host_arch, target_arch)
        if not sdk_bat_file:
            debug("sdk batch file not found:%s", _sdk)
        else:
            sdk_bat_file_path = os.path.join(sdk_pdir, sdk_bat_file)
            if os.path.exists(sdk_bat_file_path):
                debug('sdk_bat_file_path:%s', sdk_bat_file_path)
                return sdk_bat_file_path

    return None

__INSTALLED_VCS_RUN = None

def _reset_installed_vcs():
    global __INSTALLED_VCS_RUN
    debug('reset __INSTALLED_VCS_RUN')
    __INSTALLED_VCS_RUN = None

_VC_TOOLS_VERSION_FILE_PATH = ['Auxiliary', 'Build', 'Microsoft.VCToolsVersion.default.txt']
_VC_TOOLS_VERSION_FILE = os.sep.join(_VC_TOOLS_VERSION_FILE_PATH)

def _check_files_exist_in_vc_dir(env, vc_dir, msvc_version) -> bool:
    """Return status of finding batch file and cl.exe to use.

    Locates required vcvars batch files and cl in the vc_dir depending on
    TARGET_ARCH, HOST_ARCH and the msvc version. TARGET_ARCH and HOST_ARCH
    can be extracted from the passed env, unless the env is None, in which
    case the native platform is assumed for the host and all associated
    targets.

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

    # Find the host, target, and all candidate (host, target) platform combinations:
    platforms = get_host_target(env, msvc_version, all_host_targets=True)
    debug("host_platform %s, target_platform %s host_target_list %s", *platforms)
    host_platform, target_platform, host_target_list = platforms

    vernum = float(get_msvc_version_numeric(msvc_version))
    vernum_int = int(vernum * 10)

    # make sure the cl.exe exists meaning the tool is installed
    if vernum_int >= 141:
        # 14.1 (VS2017) and later
        # 2017 and newer allowed multiple versions of the VC toolset to be
        # installed at the same time. This changes the layout.
        # Just get the default tool version for now
        # TODO: support setting a specific minor VC version
        default_toolset_file = os.path.join(vc_dir, _VC_TOOLS_VERSION_FILE)
        try:
            with open(default_toolset_file) as f:
                vc_specific_version = f.readlines()[0].strip()
        except OSError:
            debug('failed to read %s', default_toolset_file)
            return False
        except IndexError:
            debug('failed to find MSVC version in %s', default_toolset_file)
            return False

        if vernum_int >= 143:
            # 14.3 (VS2022) and later
            host_target_batchfile_clpathcomps = _GE2022_HOST_TARGET_BATCHFILE_CLPATHCOMPS
        else:
            # 14.2 (VS2019) to 14.1 (VS2017)
            host_target_batchfile_clpathcomps = _LE2019_HOST_TARGET_BATCHFILE_CLPATHCOMPS

        for host_platform, target_platform in host_target_list:

            debug('host platform %s, target platform %s for version %s', host_platform, target_platform, msvc_version)

            batchfile_clpathcomps = host_target_batchfile_clpathcomps.get((host_platform, target_platform), None)
            if batchfile_clpathcomps is None:
                debug('unsupported host/target platform combo: (%s,%s)', host_platform, target_platform)
                continue

            batfile, cl_path_comps = batchfile_clpathcomps

            batfile_path = os.path.join(vc_dir, "Auxiliary", "Build", batfile)
            if not os.path.exists(batfile_path):
                debug("batch file not found: %s", batfile_path)
                continue

            cl_path = os.path.join(vc_dir, 'Tools', 'MSVC', vc_specific_version, *cl_path_comps, _CL_EXE_NAME)
            if not os.path.exists(cl_path):
                debug("%s not found: %s", _CL_EXE_NAME, cl_path)
                continue

            debug('%s found: %s', _CL_EXE_NAME, cl_path)
            return True

    elif 141 > vernum_int >= 80:
        # 14.0 (VS2015) to 8.0 (VS2005)

        if vernum_int >= 100:
            # 14.0 (VS2015) to 10.0 (VS2010)
            host_target_batcharg_batchfile_clpathcomps = _LE2015_HOST_TARGET_BATCHARG_BATCHFILE_CLPATHCOMPS
        else:
            # 9.0 (VS2008) to 8.0 (VS2005)
            host_target_batcharg_batchfile_clpathcomps = _LE2008_HOST_TARGET_BATCHARG_BATCHFILE_CLPATHCOMPS

        if vernum_int == 90 and MSVC.Kind.msvc_version_is_vcforpython(msvc_version):
            # 9.0 (VS2008) Visual C++ for Python:
            #     vcvarsall batch file is in installdir not productdir (e.g., vc\..\vcvarsall.bat)
            #     dependency batch files are not called from vcvarsall.bat
            batfile_path = os.path.join(vc_dir, os.pardir, "vcvarsall.bat")
            check_depbat = False
        else:
            batfile_path = os.path.join(vc_dir, "vcvarsall.bat")
            check_depbat = True

        if not os.path.exists(batfile_path):
            debug("batch file not found: %s", batfile_path)
            return False

        for host_platform, target_platform in host_target_list:

            debug('host platform %s, target platform %s for version %s', host_platform, target_platform, msvc_version)

            batcharg_batchfile_clpathcomps = host_target_batcharg_batchfile_clpathcomps.get(
                (host_platform, target_platform), None
            )

            if batcharg_batchfile_clpathcomps is None:
                debug('unsupported host/target platform combo: (%s,%s)', host_platform, target_platform)
                continue

            _, batfile, cl_path_comps = batcharg_batchfile_clpathcomps

            if check_depbat:
                batfile_path = os.path.join(vc_dir, *cl_path_comps, batfile)
                if not os.path.exists(batfile_path):
                    debug("batch file not found: %s", batfile_path)
                    continue

            cl_path = os.path.join(vc_dir, *cl_path_comps, _CL_EXE_NAME)
            if not os.path.exists(cl_path):
                debug("%s not found: %s", _CL_EXE_NAME, cl_path)
                continue

            debug('%s found: %s', _CL_EXE_NAME, cl_path)
            return True

    elif 80 > vernum_int >= 60:
        # 7.1 (VS2003) to 6.0 (VS6)

        # quick check for vc_dir/bin and vc_dir/ before walk
        # need to check root as the walk only considers subdirectories
        for cl_dir in ('bin', ''):
            cl_path = os.path.join(vc_dir, cl_dir, _CL_EXE_NAME)
            if os.path.exists(cl_path):
                debug('%s found %s', _CL_EXE_NAME, cl_path)
                return True
        # not in bin or root: must be in a subdirectory
        for cl_root, cl_dirs, _ in os.walk(vc_dir):
            for cl_dir in cl_dirs:
                cl_path = os.path.join(cl_root, cl_dir, _CL_EXE_NAME)
                if os.path.exists(cl_path):
                    debug('%s found: %s', _CL_EXE_NAME, cl_path)
                    return True
        return False

    else:
        # version not supported return false
        debug('unsupported MSVC version: %s', str(vernum))

    return False

def get_installed_vcs(env=None):
    global __INSTALLED_VCS_RUN

    _VSWhereExecutable.vswhere_freeze_env(env)

    if __INSTALLED_VCS_RUN is not None:
        return __INSTALLED_VCS_RUN

    save_target_arch = env.get('TARGET_ARCH', UNDEFINED) if env else None
    force_target = env and save_target_arch and save_target_arch != UNDEFINED

    if force_target:
        del env['TARGET_ARCH']
        debug("delete env['TARGET_ARCH']")

    installed_versions = []

    for ver in _VCVER:
        debug('trying to find VC %s', ver)
        try:
            VC_DIR = find_vc_pdir(ver, env)
            if VC_DIR:
                debug('found VC %s', ver)
                if _check_files_exist_in_vc_dir(env, VC_DIR, ver):
                    installed_versions.append(ver)
                else:
                    debug('no compiler found %s', ver)
            else:
                debug('return None for ver %s', ver)
        except (MSVCUnsupportedTargetArch, MSVCUnsupportedHostArch, VSWhereUserError):
            # Allow these exceptions to propagate further as it should cause
            # SCons to exit with an error code
            raise
        except VisualCException as e:
            debug('did not find VC %s: caught exception %s', ver, str(e))

    if force_target:
        env['TARGET_ARCH'] = save_target_arch
        debug("restore env['TARGET_ARCH']=%s", save_target_arch)

    __INSTALLED_VCS_RUN = installed_versions
    debug("__INSTALLED_VCS_RUN=%s", __INSTALLED_VCS_RUN)
    return __INSTALLED_VCS_RUN

def reset_installed_vcs() -> None:
    """Make it try again to find VC.  This is just for the tests."""
    _reset_installed_vcs()
    _reset_vc_pdir()
    _VSWhereExecutable.reset()
    _VSWhere.reset()
    MSVC._reset()

def msvc_default_version(env=None):
    """Get default msvc version."""
    vcs = get_installed_vcs(env)
    msvc_version = vcs[0] if vcs else None
    debug('msvc_version=%s', repr(msvc_version))
    return msvc_version

def get_installed_vcs_components(env=None):
    """Test suite convenience function: return list of installed msvc version component tuples"""
    vcs = get_installed_vcs(env)
    msvc_version_component_defs = [MSVC.Util.msvc_version_components(vcver) for vcver in vcs]
    return msvc_version_component_defs

def _check_cl_exists_in_script_env(data):
    """Find cl.exe in the script environment path."""
    cl_path = None
    if data and 'PATH' in data:
        for p in data['PATH']:
            cl_exe = os.path.join(p, _CL_EXE_NAME)
            if os.path.exists(cl_exe):
                cl_path = cl_exe
                break
    have_cl = True if cl_path else False
    debug('have_cl: %s, cl_path: %s', have_cl, cl_path)
    return have_cl, cl_path

# Running these batch files isn't cheap: most of the time spent in
# msvs.generate() is due to vcvars*.bat.  In a build that uses "tools='msvs'"
# in multiple environments, for example:
#    env1 = Environment(tools='msvs')
#    env2 = Environment(tools='msvs')
# we can greatly improve the speed of the second and subsequent Environment
# (or Clone) calls by memoizing the environment variables set by vcvars*.bat.
#
# Updated: by 2018, vcvarsall.bat had gotten so expensive (vs2017 era)
# it was breaking CI builds because the test suite starts scons so many
# times and the existing memo logic only helped with repeated calls
# within the same scons run. Windows builds on the CI system were split
# into chunks to get around single-build time limits.
# With VS2019 it got even slower and an optional persistent cache file
# was introduced. The cache now also stores only the parsed vars,
# not the entire output of running the batch file - saves a bit
# of time not parsing every time.

script_env_cache = None

def script_env(env, script, args=None):
    global script_env_cache

    if script_env_cache is None:
        script_env_cache = common.read_script_env_cache()
    cache_key = (script, args if args else None)
    cache_data = script_env_cache.get(cache_key, None)

    # Brief sanity check: if we got a value for the key,
    # see if it has a VCToolsInstallDir entry that is not empty.
    # If so, and that path does not exist, invalidate the entry.
    # If empty, this is an old compiler, just leave it alone.
    if cache_data is not None:
        try:
            toolsdir = cache_data["VCToolsInstallDir"]
        except KeyError:
            # we write this value, so should not happen
            pass
        else:
            if toolsdir:
                toolpath = Path(toolsdir[0])
                if not toolpath.exists():
                    cache_data = None

    if cache_data is None:
        skip_sendtelemetry = _skip_sendtelemetry(env)
        stdout = common.get_output(script, args, skip_sendtelemetry=skip_sendtelemetry)
        cache_data = common.parse_output(stdout)

        # debug(stdout)
        olines = stdout.splitlines()

        # process stdout: batch file errors (not necessarily first line)
        script_errlog = []
        for line in olines:
            if re_script_output_error.match(line):
                if not script_errlog:
                    script_errlog.append('vc script errors detected:')
                script_errlog.append(line)

        if script_errlog:
            script_errmsg = '\n'.join(script_errlog)

            have_cl, _ = _check_cl_exists_in_script_env(cache_data)

            debug(
                'script=%s args=%s have_cl=%s, errors=%s',
                repr(script), repr(args), repr(have_cl), script_errmsg
            )
            MSVC.Policy.msvc_scripterror_handler(env, script_errmsg)

            if not have_cl:
                # detected errors, cl.exe not on path
                raise BatchFileExecutionError(script_errmsg)

        # once we updated cache, give a chance to write out if user wanted
        script_env_cache[cache_key] = cache_data
        common.write_script_env_cache(script_env_cache)

    return cache_data

def get_default_version(env):
    msvc_version = env.get('MSVC_VERSION')
    msvs_version = env.get('MSVS_VERSION')
    debug('msvc_version:%s msvs_version:%s', msvc_version, msvs_version)

    if msvs_version and not msvc_version:
        SCons.Warnings.warn(
                SCons.Warnings.DeprecatedWarning,
                "MSVS_VERSION is deprecated: please use MSVC_VERSION instead ")
        return msvs_version
    elif msvc_version and msvs_version:
        if not msvc_version == msvs_version:
            SCons.Warnings.warn(
                    SCons.Warnings.VisualVersionMismatch,
                    "Requested msvc version (%s) and msvs version (%s) do "
                    "not match: please use MSVC_VERSION only to request a "
                    "visual studio version, MSVS_VERSION is deprecated"
                    % (msvc_version, msvs_version))
        return msvs_version

    if not msvc_version:
        msvc_version = msvc_default_version(env)
        if not msvc_version:
            #SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, msg)
            debug('No installed VCs')
            return None
        debug('using default installed MSVC version %s', repr(msvc_version))
    else:
        debug('using specified MSVC version %s', repr(msvc_version))

    return msvc_version

def msvc_setup_env_once(env, tool=None) -> None:
    try:
        has_run = env["MSVC_SETUP_RUN"]
    except KeyError:
        has_run = False

    if not has_run:
        MSVC.SetupEnvDefault.register_setup(env, msvc_exists)
        msvc_setup_env(env)
        env["MSVC_SETUP_RUN"] = True

    req_tools = MSVC.SetupEnvDefault.register_iserror(env, tool, msvc_exists)
    if req_tools:
        msg = "No versions of the MSVC compiler were found.\n" \
              "  Visual Studio C/C++ compilers may not be set correctly.\n" \
              "  Requested tool(s) are: {}".format(req_tools)
        MSVC.Policy.msvc_notfound_handler(env, msg)

def msvc_find_valid_batch_script(env, version):
    """Find and execute appropriate batch script to set up build env.

    The MSVC build environment depends heavily on having the shell
    environment set.  SCons does not inherit that, and does not count
    on that being set up correctly anyway, so it tries to find the right
    MSVC batch script, or the right arguments to the generic batch script
    vcvarsall.bat, and run that, so we have a valid environment to build in.
    There are dragons here: the batch scripts don't fail (see comments
    elsewhere), they just leave you with a bad setup, so try hard to
    get it right.
    """

    # Find the product directory
    pdir = None
    try:
        pdir = find_vc_pdir(version, env)
    except UnsupportedVersion:
        # Unsupported msvc version (raise MSVCArgumentError?)
        pass
    debug('product directory: version=%s, pdir=%s', version, pdir)

    # Find the host, target, and all candidate (host, target) platform combinations:
    platforms = get_host_target(env, version)
    debug("host_platform %s, target_platform %s host_target_list %s", *platforms)
    host_platform, target_platform, host_target_list = platforms

    d = None
    version_installed = False

    if pdir:

        # Query all candidate sdk (host, target, sdk_pdir) after vc_script pass if necessary
        sdk_queries = []

        for host_arch, target_arch, in host_target_list:
            # Set to current arch.
            env['TARGET_ARCH'] = target_arch
            arg = ''

            # Try to locate a batch file for this host/target platform combo
            try:
                (vc_script, arg, vc_dir, sdk_pdir) = find_batch_file(version, host_arch, target_arch, pdir)
                debug('vc_script:%s vc_script_arg:%s', vc_script, arg)
                version_installed = True
            except VisualCException as e:
                msg = str(e)
                debug('Caught exception while looking for batch file (%s)', msg)
                version_installed = False
                continue

            # Save (host, target, sdk_pdir) platform combo for sdk queries
            if sdk_pdir:
                sdk_query = (host_arch, target_arch, sdk_pdir)
                if sdk_query not in sdk_queries:
                    debug('save sdk_query host=%s, target=%s, sdk_pdir=%s', host_arch, target_arch, sdk_pdir)
                    sdk_queries.append(sdk_query)

            if not vc_script:
                continue

            if not target_platform and MSVC.ScriptArguments.msvc_script_arguments_has_uwp(env):
                # no target arch specified and is a store/uwp build
                if MSVC.Kind.msvc_version_skip_uwp_target(env, version):
                    # store/uwp may not be supported for all express targets (prevent constraint error)
                    debug('skip uwp target arch: version=%s, target=%s', repr(version), repr(target_arch))
                    continue

            # Try to use the located batch file for this host/target platform combo
            arg = MSVC.ScriptArguments.msvc_script_arguments(env, version, vc_dir, arg)
            debug('trying vc_script:%s, vc_script_args:%s', repr(vc_script), arg)
            try:
                d = script_env(env, vc_script, args=arg)
            except BatchFileExecutionError as e:
                debug('failed vc_script:%s, vc_script_args:%s, error:%s', repr(vc_script), arg, e)
                vc_script = None
                continue

            have_cl, _ = _check_cl_exists_in_script_env(d)
            if not have_cl:
                debug('skip cl.exe not found vc_script:%s, vc_script_args:%s', repr(vc_script), arg)
                continue

            debug("Found a working script/target: %s/%s", repr(vc_script), arg)
            break # We've found a working target_platform, so stop looking

        if not d:
            for host_arch, target_arch, sdk_pdir in sdk_queries:
                # Set to current arch.
                env['TARGET_ARCH'] = target_arch

                sdk_script = find_batch_file_sdk(host_arch, target_arch, sdk_pdir)
                if not sdk_script:
                    continue

                # Try to use the sdk batch file for this (host, target, sdk_pdir) combo
                debug('trying sdk_script:%s', repr(sdk_script))
                try:
                    d = script_env(env, sdk_script)
                    version_installed = True
                except BatchFileExecutionError as e:
                    debug('failed sdk_script:%s, error=%s', repr(sdk_script), e)
                    continue

                have_cl, _ = _check_cl_exists_in_script_env(d)
                if not have_cl:
                    debug('skip cl.exe not found sdk_script:%s', repr(sdk_script))
                    continue

                debug("Found a working script/target: %s", repr(sdk_script))
                break # We've found a working script, so stop looking

    # If we cannot find a viable installed compiler, reset the TARGET_ARCH
    # To it's initial value
    if not d:
        env['TARGET_ARCH'] = target_platform

        if version_installed:
            msg = "MSVC version '{}' working host/target script was not found.\n" \
                  "  Host = '{}', Target = '{}'\n" \
                  "  Visual Studio C/C++ compilers may not be set correctly".format(
                     version, host_platform, target_platform
                  )
        else:
            installed_vcs = get_installed_vcs(env)
            if installed_vcs:
                msg = "MSVC version '{}' was not found.\n" \
                      "  Visual Studio C/C++ compilers may not be set correctly.\n" \
                      "  Installed versions are: {}".format(version, installed_vcs)
            else:
                msg = "MSVC version '{}' was not found.\n" \
                      "  No versions of the MSVC compiler were found.\n" \
                      "  Visual Studio C/C++ compilers may not be set correctly".format(version)

        MSVC.Policy.msvc_notfound_handler(env, msg)

    return d

def get_use_script_use_settings(env):

    #   use_script  use_settings   return values   action
    #     value       ignored      (value, None)   use script or bypass detection
    #   undefined  value not None  (False, value)  use dictionary
    #   undefined  undefined/None  (True,  None)   msvc detection

    # None (documentation) or evaluates False (code): bypass detection
    # need to distinguish between undefined and None
    use_script = env.get('MSVC_USE_SCRIPT', UNDEFINED)

    if use_script != UNDEFINED:
        # use_script defined, use_settings ignored (not type checked)
        return use_script, None

    # undefined or None: use_settings ignored
    use_settings = env.get('MSVC_USE_SETTINGS', None)

    if use_settings is not None:
        # use script undefined, use_settings defined and not None (type checked)
        return False, use_settings

    # use script undefined, use_settings undefined or None
    return True, None

def msvc_setup_env(env):
    debug('called')

    _VSWhereExecutable.vswhere_freeze_env(env)

    version = get_default_version(env)
    if version is None:
        if not msvc_setup_env_user(env):
            MSVC.SetupEnvDefault.set_nodefault()
        return None

    # XXX: we set-up both MSVS version for backward
    # compatibility with the msvs tool
    env['MSVC_VERSION'] = version
    env['MSVS_VERSION'] = version
    env['MSVS'] = {}

    use_script, use_settings = get_use_script_use_settings(env)
    if SCons.Util.is_String(use_script):
        use_script = use_script.strip()
        if not os.path.exists(use_script):
            raise MSVCScriptNotFound(f'Script specified by MSVC_USE_SCRIPT not found: "{use_script}"')
        args = env.subst('$MSVC_USE_SCRIPT_ARGS')
        debug('use_script 1 %s %s', repr(use_script), repr(args))
        d = script_env(env, use_script, args)
    elif use_script:
        d = msvc_find_valid_batch_script(env,version)
        debug('use_script 2 %s', d)
        if not d:
            return d
    elif use_settings is not None:
        if not SCons.Util.is_Dict(use_settings):
            error_msg = f'MSVC_USE_SETTINGS type error: expected a dictionary, found {type(use_settings).__name__}'
            raise MSVCUseSettingsError(error_msg)
        d = use_settings
        debug('use_settings %s', d)
    else:
        debug('MSVC_USE_SCRIPT set to False')
        warn_msg = "MSVC_USE_SCRIPT set to False, assuming environment " \
                   "set correctly."
        SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, warn_msg)
        return None

    found_cl_path = None
    found_cl_envpath = None

    seen_path = False
    for k, v in d.items():
        if not seen_path and k == 'PATH':
            seen_path = True
            found_cl_path = SCons.Util.WhereIs('cl', v)
            found_cl_envpath = SCons.Util.WhereIs('cl', env['ENV'].get(k, []))
        env.PrependENVPath(k, v, delete_existing=True)
        debug("env['ENV']['%s'] = %s", k, env['ENV'][k])

    debug("cl paths: d['PATH']=%s, ENV['PATH']=%s", repr(found_cl_path), repr(found_cl_envpath))

    # final check to issue a warning if the requested compiler is not present
    if not found_cl_path:
        warn_msg = "Could not find requested MSVC compiler 'cl'."
        if CONFIG_CACHE:
            warn_msg += f" SCONS_CACHE_MSVC_CONFIG caching enabled, remove cache file {CONFIG_CACHE} if out of date."
        else:
            warn_msg += " It may need to be installed separately with Visual Studio."
        if found_cl_envpath:
            warn_msg += " A 'cl' was found on the scons ENV path which may be erroneous."
        debug(warn_msg)
        SCons.Warnings.warn(SCons.Warnings.VisualCMissingWarning, warn_msg)

def msvc_exists(env=None, version=None):
    vcs = get_installed_vcs(env)
    if version is None:
        rval = len(vcs) > 0
    else:
        rval = version in vcs
    debug('version=%s, return=%s', repr(version), rval)
    return rval

def msvc_setup_env_user(env=None):
    rval = False
    if env:

        # Intent is to use msvc tools:
        #     MSVC_VERSION:         defined and evaluates True
        #     MSVS_VERSION:         defined and evaluates True
        #     MSVC_USE_SCRIPT:      defined and (is string or evaluates False)
        #     MSVC_USE_SETTINGS:    defined and is not None

        # defined and is True
        for key in ['MSVC_VERSION', 'MSVS_VERSION']:
            if key in env and env[key]:
                rval = True
                debug('key=%s, return=%s', repr(key), rval)
                return rval

        # defined and (is string or is False)
        for key in ['MSVC_USE_SCRIPT']:
            if key in env and (SCons.Util.is_String(env[key]) or not env[key]):
                rval = True
                debug('key=%s, return=%s', repr(key), rval)
                return rval

        # defined and is not None
        for key in ['MSVC_USE_SETTINGS']:
            if key in env and env[key] is not None:
                rval = True
                debug('key=%s, return=%s', repr(key), rval)
                return rval

    debug('return=%s', rval)
    return rval

def msvc_setup_env_tool(env=None, version=None, tool=None):
    MSVC.SetupEnvDefault.register_tool(env, tool, msvc_exists)
    rval = False
    if not rval and msvc_exists(env, version):
        rval = True
    if not rval and msvc_setup_env_user(env):
        rval = True
    return rval

def msvc_sdk_versions(version=None, msvc_uwp_app: bool=False):
    debug('version=%s, msvc_uwp_app=%s', repr(version), repr(msvc_uwp_app))

    rval = []

    if not version:
        version = msvc_default_version()

    if not version:
        debug('no msvc versions detected')
        return rval

    version_def = MSVC.Util.msvc_extended_version_components(version)
    if not version_def:
        msg = f'Unsupported version {version!r}'
        raise MSVCArgumentError(msg)

    rval = MSVC.WinSDK.get_msvc_sdk_version_list(version_def.msvc_version, msvc_uwp_app)
    return rval

def msvc_toolset_versions(msvc_version=None, full: bool=True, sxs: bool=False, vswhere_exe=None):
    debug(
        'msvc_version=%s, full=%s, sxs=%s, vswhere_exe=%s',
        repr(msvc_version), repr(full), repr(sxs), repr(vswhere_exe)
    )

    _VSWhereExecutable.vswhere_freeze_executable(vswhere_exe)

    rval = []

    if not msvc_version:
        msvc_version = msvc_default_version()

    if not msvc_version:
        debug('no msvc versions detected')
        return rval

    if msvc_version not in _VCVER:
        msg = f'Unsupported msvc version {msvc_version!r}'
        raise MSVCArgumentError(msg)

    vc_dir = _find_vc_pdir(msvc_version, vswhere_exe)
    if not vc_dir:
        debug('VC folder not found for version %s', repr(msvc_version))
        return rval

    rval = MSVC.ScriptArguments._msvc_toolset_versions_internal(msvc_version, vc_dir, full=full, sxs=sxs)
    return rval

def msvc_toolset_versions_spectre(msvc_version=None, vswhere_exe=None):
    debug('msvc_version=%s, vswhere_exe=%s', repr(msvc_version), repr(vswhere_exe))

    _VSWhereExecutable.vswhere_freeze_executable(vswhere_exe)

    rval = []

    if not msvc_version:
        msvc_version = msvc_default_version()

    if not msvc_version:
        debug('no msvc versions detected')
        return rval

    if msvc_version not in _VCVER:
        msg = f'Unsupported msvc version {msvc_version!r}'
        raise MSVCArgumentError(msg)

    vc_dir = _find_vc_pdir(msvc_version, vswhere_exe)
    if not vc_dir:
        debug('VC folder not found for version %s', repr(msvc_version))
        return rval

    rval = MSVC.ScriptArguments._msvc_toolset_versions_spectre_internal(msvc_version, vc_dir)
    return rval

_InstalledVersionToolset = namedtuple('_InstalledVersionToolset', [
    'msvc_version_def',
    'toolset_version_def',
])

_InstalledVCSToolsetsComponents = namedtuple('_InstalledVCSToolsetComponents', [
    'sxs_map',
    'toolset_vcs',
    'msvc_toolset_component_defs',
])

def get_installed_vcs_toolsets_components(vswhere_exe=None):
    debug('vswhere_exe=%s', repr(vswhere_exe))

    _VSWhereExecutable.vswhere_freeze_executable(vswhere_exe)

    sxs_map = {}
    toolset_vcs = set()
    msvc_toolset_component_defs = []

    vcs = get_installed_vcs()
    for msvc_version in vcs:

        vc_dir = _find_vc_pdir(msvc_version, vswhere_exe)
        if not vc_dir:
            continue

        msvc_version_def = MSVC.Util.msvc_version_components(msvc_version)
        toolset_vcs.add(msvc_version_def.msvc_version)

        if msvc_version_def.msvc_vernum > 14.0:

            toolsets_sxs, toolsets_full = MSVC.ScriptArguments._msvc_version_toolsets_internal(
                msvc_version, vc_dir
            )

            debug('msvc_version=%s, toolset_sxs=%s', repr(msvc_version), repr(toolsets_sxs))
            sxs_map.update(toolsets_sxs)

        else:

            toolsets_full = [msvc_version_def.msvc_verstr]

        for toolset_version in toolsets_full:
            debug('msvc_version=%s, toolset_version=%s', repr(msvc_version), repr(toolset_version))

            toolset_version_def = MSVC.Util.msvc_extended_version_components(toolset_version)
            if not toolset_version_def:
                continue
            toolset_vcs.add(toolset_version_def.msvc_version)

            rval = _InstalledVersionToolset(
                msvc_version_def=msvc_version_def,
                toolset_version_def=toolset_version_def,
            )
            msvc_toolset_component_defs.append(rval)

    installed_vcs_toolsets_components = _InstalledVCSToolsetsComponents(
        sxs_map=sxs_map,
        toolset_vcs=toolset_vcs,
        msvc_toolset_component_defs=msvc_toolset_component_defs,
    )

    return installed_vcs_toolsets_components

def msvc_query_version_toolset(version=None, prefer_newest: bool=True, vswhere_exe=None):
    """
    Return an msvc version and a toolset version given a version
    specification.

    This is an EXPERIMENTAL proxy for using a toolset version to perform
    msvc instance selection.  This function will be removed when
    toolset version is taken into account during msvc instance selection.

    This function searches for an installed Visual Studio instance that
    contains the requested version. A component suffix (e.g., Exp) is not
    supported for toolset version specifications (e.g., 14.39).

    An MSVCArgumentError is raised when argument validation fails. An
    MSVCToolsetVersionNotFound exception is raised when the requested
    version is not found.

    For Visual Studio 2015 and earlier, the msvc version is returned and
    the toolset version is None.  For Visual Studio 2017 and later, the
    selected toolset version is returned.

    Args:

        version: str
            The version specification may be an msvc version or a toolset
            version.

        prefer_newest: bool
            True:  prefer newer Visual Studio instances.
            False: prefer the "native" Visual Studio instance first. If
                   the native Visual Studio instance is not detected, prefer
                   newer Visual Studio instances.

        vswhere_exe: str
            A vswhere executable location or None.

    Returns:
        tuple: A tuple containing the msvc version and the msvc toolset version.
               The msvc toolset version may be None.

    Raises:
        MSVCToolsetVersionNotFound: when the specified version is not found.
        MSVCArgumentError: when argument validation fails.
    """
    debug(
        'version=%s, prefer_newest=%s, vswhere_exe=%s',
        repr(version), repr(prefer_newest), repr(vswhere_exe)
    )

    _VSWhereExecutable.vswhere_freeze_executable(vswhere_exe)

    msvc_version = None
    msvc_toolset_version = None

    with MSVC.Policy.msvc_notfound_policy_contextmanager('suppress'):

        vcs = get_installed_vcs()

        if not version:
            version = msvc_default_version()

        if not version:
            msg = f'No versions of the MSVC compiler were found'
            debug(f'MSVCToolsetVersionNotFound: {msg}')
            raise MSVCToolsetVersionNotFound(msg)

        version_def = MSVC.Util.msvc_extended_version_components(version)

        if not version_def:
            msg = f'Unsupported MSVC version {version!r}'
            debug(f'MSVCArgumentError: {msg}')
            raise MSVCArgumentError(msg)

        msvc_version = version_def.msvc_version

        if version_def.msvc_suffix:

            if version_def.msvc_verstr != version_def.msvc_toolset_version:
                # toolset version with component suffix
                msg = f'Unsupported MSVC toolset version {version!r}'
                debug(f'MSVCArgumentError: {msg}')
                raise MSVCArgumentError(msg)

            if msvc_version not in vcs:
                msg = f'MSVC version {msvc_version!r} not found'
                debug(f'MSVCToolsetVersionNotFound: {msg}')
                raise MSVCToolsetVersionNotFound(msg)

            if msvc_version not in MSVC.Config.MSVC_VERSION_TOOLSET_SEARCH_MAP:
                debug(
                    'suffix: msvc_version=%s, msvc_toolset_version=%s',
                    repr(msvc_version), repr(msvc_toolset_version)
                )
                return msvc_version, msvc_toolset_version

        if version_def.msvc_vernum > 14.0:
            # VS2017 and later
            force_toolset_msvc_version = False
        else:
            # VS2015 and earlier
            force_toolset_msvc_version = True
            extended_version = version_def.msvc_verstr + '0.00000'
            if not extended_version.startswith(version_def.msvc_toolset_version):
                # toolset not equivalent to msvc version
                msg = 'Unsupported MSVC toolset version {} (expected {})'.format(
                    repr(version), repr(extended_version)
                )
                debug(f'MSVCArgumentError: {msg}')
                raise MSVCArgumentError(msg)

        if msvc_version not in MSVC.Config.MSVC_VERSION_TOOLSET_SEARCH_MAP:

            # VS2013 and earlier

            if msvc_version not in vcs:
                msg = f'MSVC version {version!r} not found'
                debug(f'MSVCToolsetVersionNotFound: {msg}')
                raise MSVCToolsetVersionNotFound(msg)

            debug(
                'earlier: msvc_version=%s, msvc_toolset_version=%s',
                repr(msvc_version), repr(msvc_toolset_version)
            )
            return msvc_version, msvc_toolset_version

        if force_toolset_msvc_version:
            query_msvc_toolset_version = version_def.msvc_verstr
        else:
            query_msvc_toolset_version = version_def.msvc_toolset_version

        if prefer_newest:
            query_version_list = MSVC.Config.MSVC_VERSION_TOOLSET_SEARCH_MAP[msvc_version]
        else:
            query_version_list = MSVC.Config.MSVC_VERSION_TOOLSET_DEFAULTS_MAP[msvc_version] + \
                                 MSVC.Config.MSVC_VERSION_TOOLSET_SEARCH_MAP[msvc_version]

        seen_msvc_version = set()
        for query_msvc_version in query_version_list:

            if query_msvc_version in seen_msvc_version:
                continue
            seen_msvc_version.add(query_msvc_version)

            vc_dir = _find_vc_pdir(query_msvc_version, vswhere_exe)
            if not vc_dir:
                continue

            if query_msvc_version.startswith('14.0'):
                # VS2015 does not support toolset version argument
                msvc_toolset_version = None
                debug(
                    'found: msvc_version=%s, msvc_toolset_version=%s',
                    repr(query_msvc_version), repr(msvc_toolset_version)
                )
                return query_msvc_version, msvc_toolset_version

            try:
                toolset_vcvars = MSVC.ScriptArguments._msvc_toolset_internal(query_msvc_version, query_msvc_toolset_version, vc_dir)
                if toolset_vcvars:
                    msvc_toolset_version = toolset_vcvars
                    debug(
                        'found: msvc_version=%s, msvc_toolset_version=%s',
                        repr(query_msvc_version), repr(msvc_toolset_version)
                    )
                    return query_msvc_version, msvc_toolset_version

            except MSVCToolsetVersionNotFound:
                pass

        msvc_toolset_version = query_msvc_toolset_version

    debug(
        'not found: msvc_version=%s, msvc_toolset_version=%s',
        repr(msvc_version), repr(msvc_toolset_version)
    )

    msg = f'MSVC toolset version {version!r} not found'
    debug(f'MSVCToolsetVersionNotFound: {msg}')
    raise MSVCToolsetVersionNotFound(msg)


# internal consistency check (should be last)
MSVC._verify()

