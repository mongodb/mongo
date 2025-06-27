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
Batch file argument functions for Microsoft Visual C/C++.
"""

import os
import re
import enum

from collections import (
    namedtuple,
)

from ..common import (
    CONFIG_CACHE_FORCE_DEFAULT_ARGUMENTS,
    debug,
)

from . import Util
from . import Config
from . import Registry
from . import WinSDK
from . import Kind

from .Exceptions import (
    MSVCInternalError,
    MSVCSDKVersionNotFound,
    MSVCToolsetVersionNotFound,
    MSVCSpectreLibsNotFound,
    MSVCArgumentError,
)

from . import Dispatcher
Dispatcher.register_modulename(__name__)


# Script argument: boolean True
_ARGUMENT_BOOLEAN_TRUE_LEGACY = (True, '1') # MSVC_UWP_APP
_ARGUMENT_BOOLEAN_TRUE = (True,)

# TODO: verify SDK 10 version folder names 10.0.XXXXX.0 {1,3} last?
re_sdk_version_100 = re.compile(r'^10[.][0-9][.][0-9]{5}[.][0-9]{1}$')
re_sdk_version_81 = re.compile(r'^8[.]1$')

re_sdk_dispatch_map = {
    '10.0': re_sdk_version_100,
    '8.1': re_sdk_version_81,
}

def _verify_re_sdk_dispatch_map():
    debug('')
    for sdk_version in Config.MSVC_SDK_VERSIONS:
        if sdk_version in re_sdk_dispatch_map:
            continue
        err_msg = f'sdk version {sdk_version} not in re_sdk_dispatch_map'
        raise MSVCInternalError(err_msg)
    return None

# SxS version bugfix
_msvc_sxs_bugfix_map = {}
_msvc_sxs_bugfix_folder = {}
_msvc_sxs_bugfix_version = {}

for msvc_version, sxs_version, sxs_bugfix in [
    # VS2019\Common7\Tools\vsdevcmd\ext\vcvars.bat AzDO Bug#1293526
    #     special handling of the 16.8 SxS toolset, use VC\Auxiliary\Build\14.28 directory and SxS files
    #     if SxS version 14.28 not present/installed, fallback selection of toolset VC\Tools\MSVC\14.28.nnnnn.
    ('14.2', '14.28.16.8', '14.28')
]:
    _msvc_sxs_bugfix_map.setdefault(msvc_version, []).append((sxs_version, sxs_bugfix))
    _msvc_sxs_bugfix_folder[(msvc_version, sxs_bugfix)] = sxs_version
    _msvc_sxs_bugfix_version[(msvc_version, sxs_version)] = sxs_bugfix

# MSVC_SCRIPT_ARGS
re_vcvars_uwp = re.compile(r'(?:(?<!\S)|^)(?P<uwp>(?:uwp|store))(?:(?!\S)|$)',re.IGNORECASE)
re_vcvars_sdk = re.compile(r'(?:(?<!\S)|^)(?P<sdk>(?:[1-9][0-9]*[.]\S*))(?:(?!\S)|$)',re.IGNORECASE)
re_vcvars_toolset = re.compile(r'(?:(?<!\S)|^)(?P<toolset_arg>(?:[-]{1,2}|[/])vcvars_ver[=](?P<toolset>\S*))(?:(?!\S)|$)', re.IGNORECASE)
re_vcvars_spectre = re.compile(r'(?:(?<!\S)|^)(?P<spectre_arg>(?:[-]{1,2}|[/])vcvars_spectre_libs[=](?P<spectre>\S*))(?:(?!\S)|$)',re.IGNORECASE)

# Force default sdk argument
_MSVC_FORCE_DEFAULT_SDK = False

# Force default toolset argument
_MSVC_FORCE_DEFAULT_TOOLSET = False

# Force default arguments
_MSVC_FORCE_DEFAULT_ARGUMENTS = False

def _msvc_force_default_sdk(force: bool=True) -> None:
    global _MSVC_FORCE_DEFAULT_SDK
    _MSVC_FORCE_DEFAULT_SDK = force
    debug('_MSVC_FORCE_DEFAULT_SDK=%s', repr(force))

def _msvc_force_default_toolset(force: bool=True) -> None:
    global _MSVC_FORCE_DEFAULT_TOOLSET
    _MSVC_FORCE_DEFAULT_TOOLSET = force
    debug('_MSVC_FORCE_DEFAULT_TOOLSET=%s', repr(force))

def msvc_force_default_arguments(force=None):
    global _MSVC_FORCE_DEFAULT_ARGUMENTS
    prev_policy = _MSVC_FORCE_DEFAULT_ARGUMENTS
    if force is not None:
        _MSVC_FORCE_DEFAULT_ARGUMENTS = force
        _msvc_force_default_sdk(force)
        _msvc_force_default_toolset(force)
    return prev_policy

if CONFIG_CACHE_FORCE_DEFAULT_ARGUMENTS:
    msvc_force_default_arguments(force=True)

# UWP SDK 8.1 and SDK 10:
#
#     https://stackoverflow.com/questions/46659238/build-windows-app-compatible-for-8-1-and-10
#     VS2019 - UWP (Except for Win10Mobile)
#     VS2017 - UWP
#     VS2015 - UWP, Win8.1 StoreApp, WP8/8.1 StoreApp
#     VS2013 - Win8/8.1 StoreApp, WP8/8.1 StoreApp

# SPECTRE LIBS (msvc documentation):
#     "There are no versions of Spectre-mitigated libraries for Universal Windows (UWP) apps or
#     components. App-local deployment of such libraries isn't possible."

# MSVC batch file arguments:
#
#     VS2022: UWP, SDK, TOOLSET, SPECTRE
#     VS2019: UWP, SDK, TOOLSET, SPECTRE
#     VS2017: UWP, SDK, TOOLSET, SPECTRE
#     VS2015: UWP, SDK
#
#     MSVC_SCRIPT_ARGS:     VS2015+
#
#     MSVC_UWP_APP:         VS2015+
#     MSVC_SDK_VERSION:     VS2015+
#     MSVC_TOOLSET_VERSION: VS2017+
#     MSVC_SPECTRE_LIBS:    VS2017+

@enum.unique
class SortOrder(enum.IntEnum):
    UWP = 1      # MSVC_UWP_APP
    SDK = 2      # MSVC_SDK_VERSION
    TOOLSET = 3  # MSVC_TOOLSET_VERSION
    SPECTRE = 4  # MSVC_SPECTRE_LIBS
    USER = 5     # MSVC_SCRIPT_ARGS

VS2019 = Config.MSVS_VERSION_INTERNAL['2019']
VS2017 = Config.MSVS_VERSION_INTERNAL['2017']
VS2015 = Config.MSVS_VERSION_INTERNAL['2015']

MSVC_VERSION_ARGS_DEFINITION = namedtuple('MSVCVersionArgsDefinition', [
    'version', # full version (e.g., '14.1Exp', '14.32.31326')
    'vs_def',
])

TOOLSET_VERSION_ARGS_DEFINITION = namedtuple('ToolsetVersionArgsDefinition', [
    'version', # full version (e.g., '14.1Exp', '14.32.31326')
    'vc_buildtools_def',
    'is_user',
])

def _msvc_version(version):

    verstr = Util.get_msvc_version_prefix(version)
    vs_def = Config.MSVC_VERSION_INTERNAL[verstr]

    version_args = MSVC_VERSION_ARGS_DEFINITION(
        version = version,
        vs_def = vs_def,
    )

    return version_args

def _toolset_version(version, is_user=False):

    vc_series = Util.get_msvc_version_prefix(version)

    vc_buildseries_def = Config.MSVC_BUILDSERIES_EXTERNAL[vc_series]
    vc_buildtools_def = Config.VC_BUILDTOOLS_MAP[vc_buildseries_def.vc_buildseries]

    version_args = TOOLSET_VERSION_ARGS_DEFINITION(
        version = version,
        vc_buildtools_def = vc_buildtools_def,
        is_user = is_user,
    )

    return version_args

def _msvc_script_argument_uwp(env, msvc, arglist, target_arch):

    uwp_app = env['MSVC_UWP_APP']
    debug('MSVC_VERSION=%s, MSVC_UWP_APP=%s', repr(msvc.version), repr(uwp_app))

    if not uwp_app:
        return None

    if uwp_app not in _ARGUMENT_BOOLEAN_TRUE_LEGACY:
        return None

    if msvc.vs_def.vc_buildtools_def.msvc_version_numeric < VS2015.vc_buildtools_def.msvc_version_numeric:
        debug(
            'invalid: msvc version constraint: %s < %s VS2015',
            repr(msvc.vs_def.vc_buildtools_def.msvc_version_numeric),
            repr(VS2015.vc_buildtools_def.msvc_version_numeric)
        )
        err_msg = "MSVC_UWP_APP ({}) constraint violation: MSVC_VERSION {} < {} VS2015".format(
            repr(uwp_app), repr(msvc.version), repr(VS2015.vc_buildtools_def.msvc_version)
        )
        raise MSVCArgumentError(err_msg)

    is_supported, is_target = Kind.msvc_version_uwp_is_supported(msvc.version, target_arch, env)
    if not is_supported:
        _, kind_str = Kind.get_msvc_version_kind(msvc.version)
        debug(
            'invalid: msvc_version constraint: %s %s %s',
            repr(msvc.version), repr(kind_str), repr(target_arch)
        )
        if is_target and target_arch:
            err_msg = "MSVC_UWP_APP ({}) TARGET_ARCH ({}) is not supported for MSVC_VERSION {} ({})".format(
                repr(uwp_app), repr(target_arch), repr(msvc.version), repr(kind_str)
            )
        else:
            err_msg = "MSVC_UWP_APP ({}) is not supported for MSVC_VERSION {} ({})".format(
                repr(uwp_app), repr(msvc.version), repr(kind_str)
            )
        raise MSVCArgumentError(err_msg)

    # VS2017+ rewrites uwp => store for 14.0 toolset
    uwp_arg = msvc.vs_def.vc_uwp

    # store/uwp may not be fully installed
    argpair = (SortOrder.UWP, uwp_arg)
    arglist.append(argpair)

    return uwp_arg

def _user_script_argument_uwp(env, uwp, user_argstr) -> bool:

    matches = [m for m in re_vcvars_uwp.finditer(user_argstr)]
    if not matches:
        return False

    if len(matches) > 1:
        debug('multiple uwp declarations: MSVC_SCRIPT_ARGS=%s', repr(user_argstr))
        err_msg = f"multiple uwp declarations: MSVC_SCRIPT_ARGS={user_argstr!r}"
        raise MSVCArgumentError(err_msg)

    if not uwp:
        return True

    env_argstr = env.get('MSVC_UWP_APP','')
    debug('multiple uwp declarations: MSVC_UWP_APP=%s, MSVC_SCRIPT_ARGS=%s', repr(env_argstr), repr(user_argstr))

    err_msg = "multiple uwp declarations: MSVC_UWP_APP={} and MSVC_SCRIPT_ARGS={}".format(
        repr(env_argstr), repr(user_argstr)
    )

    raise MSVCArgumentError(err_msg)

def _msvc_script_argument_sdk_constraints(msvc, sdk_version, env):

    if msvc.vs_def.vc_buildtools_def.msvc_version_numeric < VS2015.vc_buildtools_def.msvc_version_numeric:
        debug(
            'invalid: msvc_version constraint: %s < %s VS2015',
            repr(msvc.vs_def.vc_buildtools_def.msvc_version_numeric),
            repr(VS2015.vc_buildtools_def.msvc_version_numeric)
        )
        err_msg = "MSVC_SDK_VERSION ({}) constraint violation: MSVC_VERSION {} < {} VS2015".format(
            repr(sdk_version), repr(msvc.version), repr(VS2015.vc_buildtools_def.msvc_version)
        )
        return err_msg

    if not Kind.msvc_version_sdk_version_is_supported(msvc.version, env):
        _, kind_str = Kind.get_msvc_version_kind(msvc.version)
        debug('invalid: msvc_version constraint: %s %s', repr(msvc.version), repr(kind_str))
        err_msg = "MSVC_SDK_VERSION ({}) is not supported for MSVC_VERSION {} ({})".format(
            repr(sdk_version), repr(msvc.version), repr(kind_str)
        )
        return err_msg

    for msvc_sdk_version in msvc.vs_def.vc_sdk_versions:
        re_sdk_version = re_sdk_dispatch_map[msvc_sdk_version]
        if re_sdk_version.match(sdk_version):
            debug('valid: sdk_version=%s', repr(sdk_version))
            return None

    debug('invalid: method exit: sdk_version=%s', repr(sdk_version))
    err_msg = f"MSVC_SDK_VERSION ({sdk_version!r}) is not supported"
    return err_msg

def _msvc_script_argument_sdk_platform_constraints(msvc, toolset, sdk_version, platform_def):

    if sdk_version == '8.1' and platform_def.is_uwp:

        vc_buildtools_def = toolset.vc_buildtools_def if toolset else msvc.vs_def.vc_buildtools_def

        if vc_buildtools_def.msvc_version_numeric > VS2015.vc_buildtools_def.msvc_version_numeric:
            debug(
                'invalid: uwp/store SDK 8.1 msvc_version constraint: %s > %s VS2015',
                repr(vc_buildtools_def.msvc_version_numeric),
                repr(VS2015.vc_buildtools_def.msvc_version_numeric)
            )
            if toolset and toolset.is_user:
                err_msg = "MSVC_SDK_VERSION ({}) and platform type ({}) constraint violation: toolset {} MSVC_VERSION {} > {} VS2015".format(
                    repr(sdk_version), repr(platform_def.vc_platform),
                    repr(toolset.version), repr(msvc.version), repr(VS2015.vc_buildtools_def.msvc_version)
                )
            else:
                err_msg = "MSVC_SDK_VERSION ({}) and platform type ({}) constraint violation: MSVC_VERSION {} > {} VS2015".format(
                    repr(sdk_version), repr(platform_def.vc_platform),
                    repr(msvc.version), repr(VS2015.vc_buildtools_def.msvc_version)
                )
            return err_msg

    return None

def _msvc_script_argument_sdk(env, msvc, toolset, platform_def, arglist):

    sdk_version = env['MSVC_SDK_VERSION']
    debug(
        'MSVC_VERSION=%s, MSVC_SDK_VERSION=%s, platform_type=%s',
        repr(msvc.version), repr(sdk_version), repr(platform_def.vc_platform)
    )

    if not sdk_version:
        return None

    err_msg = _msvc_script_argument_sdk_constraints(msvc, sdk_version, env)
    if err_msg:
        raise MSVCArgumentError(err_msg)

    sdk_list = WinSDK.get_sdk_version_list(msvc.vs_def, platform_def)

    if sdk_version not in sdk_list:
        err_msg = "MSVC_SDK_VERSION {} not found for platform type {}".format(
            repr(sdk_version), repr(platform_def.vc_platform)
        )
        raise MSVCSDKVersionNotFound(err_msg)

    err_msg = _msvc_script_argument_sdk_platform_constraints(msvc, toolset, sdk_version, platform_def)
    if err_msg:
        raise MSVCArgumentError(err_msg)

    argpair = (SortOrder.SDK, sdk_version)
    arglist.append(argpair)

    return sdk_version

def _msvc_script_default_sdk(env, msvc, platform_def, arglist, force_sdk: bool=False):

    if msvc.vs_def.vc_buildtools_def.msvc_version_numeric < VS2015.vc_buildtools_def.msvc_version_numeric:
        return None

    if not Kind.msvc_version_sdk_version_is_supported(msvc.version, env):
        return None

    sdk_list = WinSDK.get_sdk_version_list(msvc.vs_def, platform_def)
    if not len(sdk_list):
        return None

    sdk_default = sdk_list[0]

    debug(
        'MSVC_VERSION=%s, sdk_default=%s, platform_type=%s',
        repr(msvc.version), repr(sdk_default), repr(platform_def.vc_platform)
    )

    if force_sdk:
        argpair = (SortOrder.SDK, sdk_default)
        arglist.append(argpair)

    return sdk_default

def _user_script_argument_sdk(env, sdk_version, user_argstr):

    matches = [m for m in re_vcvars_sdk.finditer(user_argstr)]
    if not matches:
        return None

    if len(matches) > 1:
        debug('multiple sdk version declarations: MSVC_SCRIPT_ARGS=%s', repr(user_argstr))
        err_msg = f"multiple sdk version declarations: MSVC_SCRIPT_ARGS={user_argstr!r}"
        raise MSVCArgumentError(err_msg)

    if not sdk_version:
        user_sdk = matches[0].group('sdk')
        return user_sdk

    env_argstr = env.get('MSVC_SDK_VERSION','')
    debug('multiple sdk version declarations: MSVC_SDK_VERSION=%s, MSVC_SCRIPT_ARGS=%s', repr(env_argstr), repr(user_argstr))

    err_msg = "multiple sdk version declarations: MSVC_SDK_VERSION={} and MSVC_SCRIPT_ARGS={}".format(
        repr(env_argstr), repr(user_argstr)
    )

    raise MSVCArgumentError(err_msg)

_toolset_have140_cache = None

def _msvc_have140_toolset():
    global _toolset_have140_cache

    if _toolset_have140_cache is None:
        suffix = Registry.vstudio_sxs_vc7('14.0')
        vcinstalldirs = [record[0] for record in Registry.microsoft_query_paths(suffix)]
        debug('vc140 toolset: paths=%s', repr(vcinstalldirs))
        _toolset_have140_cache = True if vcinstalldirs else False

    return _toolset_have140_cache

def _reset_have140_cache() -> None:
    global _toolset_have140_cache
    debug('reset: cache')
    _toolset_have140_cache = None

def _msvc_read_toolset_file(msvc, filename):
    toolset_version = None
    try:
        with open(filename) as f:
            toolset_version = f.readlines()[0].strip()
        debug(
            'msvc_version=%s, filename=%s, toolset_version=%s',
            repr(msvc.version), repr(filename), repr(toolset_version)
        )
    except OSError:
        debug('OSError: msvc_version=%s, filename=%s', repr(msvc.version), repr(filename))
    except IndexError:
        debug('IndexError: msvc_version=%s, filename=%s', repr(msvc.version), repr(filename))
    return toolset_version

def _msvc_sxs_toolset_folder(msvc, sxs_folder):

    if Util.is_toolset_sxs(sxs_folder):
        return sxs_folder, sxs_folder

    for vc_buildseries_def in msvc.vs_def.vc_buildtools_def.vc_buildseries_list:
        key = (vc_buildseries_def.vc_version, sxs_folder)
        sxs_version = _msvc_sxs_bugfix_folder.get(key)
        if sxs_version:
            return sxs_folder, sxs_version

    debug('sxs folder: ignore version=%s', repr(sxs_folder))
    return None, None

def _msvc_read_toolset_folders(msvc, vc_dir):

    toolsets_sxs = {}
    toolsets_full = []

    build_dir = os.path.join(vc_dir, "Auxiliary", "Build")
    if os.path.exists(build_dir):
        for sxs_folder, sxs_path in Util.listdir_dirs(build_dir):
            sxs_folder, sxs_version = _msvc_sxs_toolset_folder(msvc, sxs_folder)
            if not sxs_version:
                continue
            filename = f'Microsoft.VCToolsVersion.{sxs_folder}.txt'
            filepath = os.path.join(sxs_path, filename)
            debug('sxs toolset: check file=%s', repr(filepath))
            if os.path.exists(filepath):
                toolset_version = _msvc_read_toolset_file(msvc, filepath)
                if not toolset_version:
                    continue
                toolsets_sxs[sxs_version] = toolset_version
                debug(
                    'sxs toolset: msvc_version=%s, sxs_version=%s, toolset_version=%s',
                    repr(msvc.version), repr(sxs_version), repr(toolset_version)
                )

    toolset_dir = os.path.join(vc_dir, "Tools", "MSVC")
    if os.path.exists(toolset_dir):
        for toolset_version, toolset_path in Util.listdir_dirs(toolset_dir):
            binpath = os.path.join(toolset_path, "bin")
            debug('toolset: check binpath=%s', repr(binpath))
            if os.path.exists(binpath):
                toolsets_full.append(toolset_version)
                debug(
                    'toolset: msvc_version=%s, toolset_version=%s',
                    repr(msvc.version), repr(toolset_version)
                )

    vcvars140 = os.path.join(vc_dir, "..", "Common7", "Tools", "vsdevcmd", "ext", "vcvars", "vcvars140.bat")
    if os.path.exists(vcvars140) and _msvc_have140_toolset():
        toolset_version = '14.0'
        toolsets_full.append(toolset_version)
        debug(
            'toolset: msvc_version=%s, toolset_version=%s',
            repr(msvc.version), repr(toolset_version)
        )

    toolsets_full.sort(reverse=True)

    # SxS bugfix fixup (if necessary)
    if msvc.version in _msvc_sxs_bugfix_map:
        for sxs_version, sxs_bugfix in _msvc_sxs_bugfix_map[msvc.version]:
            if sxs_version in toolsets_sxs:
                # have SxS version (folder/file mapping exists)
                continue
            for toolset_version in toolsets_full:
                if not toolset_version.startswith(sxs_bugfix):
                    continue
                debug(
                    'sxs toolset: msvc_version=%s, sxs_version=%s, toolset_version=%s',
                    repr(msvc.version), repr(sxs_version), repr(toolset_version)
                )
                # SxS compatible bugfix version (equivalent to toolset search)
                toolsets_sxs[sxs_version] = toolset_version
                break

    debug('msvc_version=%s, toolsets=%s', repr(msvc.version), repr(toolsets_full))

    return toolsets_sxs, toolsets_full

def _msvc_read_toolset_default(msvc, vc_dir):

    build_dir = os.path.join(vc_dir, "Auxiliary", "Build")

    # VS2019+
    filename = f"Microsoft.VCToolsVersion.{msvc.vs_def.vc_buildtools_def.vc_buildtools}.default.txt"
    filepath = os.path.join(build_dir, filename)

    debug('default toolset: check file=%s', repr(filepath))
    if os.path.exists(filepath):
        toolset_buildtools = _msvc_read_toolset_file(msvc, filepath)
        if toolset_buildtools:
            return toolset_buildtools

    # VS2017+
    filename = "Microsoft.VCToolsVersion.default.txt"
    filepath = os.path.join(build_dir, filename)

    debug('default toolset: check file=%s', repr(filepath))
    if os.path.exists(filepath):
        toolset_default = _msvc_read_toolset_file(msvc, filepath)
        if toolset_default:
            return toolset_default

    return None

_toolset_version_cache = {}
_toolset_default_cache = {}

def _reset_toolset_cache() -> None:
    global _toolset_version_cache
    global _toolset_default_cache
    debug('reset: toolset cache')
    _toolset_version_cache = {}
    _toolset_default_cache = {}

def _msvc_version_toolsets(msvc, vc_dir):

    if msvc.version in _toolset_version_cache:
        toolsets_sxs, toolsets_full = _toolset_version_cache[msvc.version]
    else:
        toolsets_sxs, toolsets_full = _msvc_read_toolset_folders(msvc, vc_dir)
        _toolset_version_cache[msvc.version] = toolsets_sxs, toolsets_full

    return toolsets_sxs, toolsets_full

def _msvc_default_toolset(msvc, vc_dir):

    if msvc.version in _toolset_default_cache:
        toolset_default = _toolset_default_cache[msvc.version]
    else:
        toolset_default = _msvc_read_toolset_default(msvc, vc_dir)
        _toolset_default_cache[msvc.version] = toolset_default

    return toolset_default

def _msvc_version_toolset_vcvars(msvc, vc_dir, toolset_version):

    toolsets_sxs, toolsets_full = _msvc_version_toolsets(msvc, vc_dir)

    if toolset_version in toolsets_full:
        # full toolset version provided
        toolset_vcvars = toolset_version
        return toolset_vcvars

    if Util.is_toolset_sxs(toolset_version):
        # SxS version provided
        sxs_version = toolsets_sxs.get(toolset_version, None)
        if sxs_version and sxs_version in toolsets_full:
            # SxS full toolset version
            toolset_vcvars = sxs_version
            return toolset_vcvars
        return None

    for toolset_full in toolsets_full:
        if toolset_full.startswith(toolset_version):
            toolset_vcvars = toolset_full
            return toolset_vcvars

    return None

def _msvc_script_argument_toolset_constraints(msvc, toolset_version):

    if msvc.vs_def.vc_buildtools_def.msvc_version_numeric < VS2017.vc_buildtools_def.msvc_version_numeric:
        debug(
            'invalid: msvc version constraint: %s < %s VS2017',
            repr(msvc.vs_def.vc_buildtools_def.msvc_version_numeric),
            repr(VS2017.vc_buildtools_def.msvc_version_numeric)
        )
        err_msg = "MSVC_TOOLSET_VERSION ({}) constraint violation: MSVC_VERSION {} < {} VS2017".format(
            repr(toolset_version), repr(msvc.version), repr(VS2017.vc_buildtools_def.msvc_version)
        )
        return err_msg

    toolset_series = Util.get_msvc_version_prefix(toolset_version)

    if not toolset_series:
        debug('invalid: msvc version: toolset_version=%s', repr(toolset_version))
        err_msg = 'MSVC_TOOLSET_VERSION {} format is not supported'.format(
            repr(toolset_version)
        )
        return err_msg

    toolset_buildseries_def = Config.MSVC_BUILDSERIES_EXTERNAL.get(toolset_series)
    if not toolset_buildseries_def:
        debug('invalid: msvc version: toolset_version=%s', repr(toolset_version))
        err_msg = 'MSVC_TOOLSET_VERSION {} build series {} is not supported'.format(
            repr(toolset_version), repr(toolset_series)
        )
        return err_msg

    toolset_buildtools_def = Config.VC_BUILDTOOLS_MAP[toolset_buildseries_def.vc_buildseries]

    toolset_verstr = toolset_buildtools_def.msvc_version
    toolset_vernum = toolset_buildtools_def.msvc_version_numeric

    if toolset_vernum < VS2015.vc_buildtools_def.msvc_version_numeric:
        debug(
            'invalid: toolset version constraint: %s < %s VS2015',
            repr(toolset_vernum), repr(VS2015.vc_buildtools_def.msvc_version_numeric)
        )
        err_msg = "MSVC_TOOLSET_VERSION ({}) constraint violation: toolset msvc version {} < {} VS2015".format(
            repr(toolset_version), repr(toolset_verstr), repr(VS2015.vc_buildtools_def.msvc_version)
        )
        return err_msg

    if toolset_vernum > msvc.vs_def.vc_buildtools_def.msvc_version_numeric:
        debug(
            'invalid: toolset version constraint: toolset %s > %s msvc',
            repr(toolset_vernum), repr(msvc.vs_def.vc_buildtools_def.msvc_version_numeric)
        )
        err_msg = "MSVC_TOOLSET_VERSION ({}) constraint violation: toolset msvc version {} > {} MSVC_VERSION".format(
            repr(toolset_version), repr(toolset_verstr), repr(msvc.version)
        )
        return err_msg

    if toolset_vernum == VS2015.vc_buildtools_def.msvc_version_numeric:
        # tooset = 14.0
        if Util.is_toolset_full(toolset_version):
            if not Util.is_toolset_140(toolset_version):
                debug(
                    'invalid: toolset version 14.0 constraint: %s != 14.0',
                    repr(toolset_version)
                )
                err_msg = "MSVC_TOOLSET_VERSION ({}) constraint violation: toolset msvc version {} != '14.0'".format(
                    repr(toolset_version), repr(toolset_version)
                )
                return err_msg
            return None

    if Util.is_toolset_full(toolset_version):
        debug('valid: toolset full: toolset_version=%s', repr(toolset_version))
        return None

    if Util.is_toolset_sxs(toolset_version):
        debug('valid: toolset sxs: toolset_version=%s', repr(toolset_version))
        return None

    debug('invalid: method exit: toolset_version=%s', repr(toolset_version))
    err_msg = f"MSVC_TOOLSET_VERSION ({toolset_version!r}) format is not supported"
    return err_msg

def _msvc_script_argument_toolset_vcvars(msvc, toolset_version, vc_dir):

    err_msg = _msvc_script_argument_toolset_constraints(msvc, toolset_version)
    if err_msg:
        raise MSVCArgumentError(err_msg)

    if toolset_version.startswith('14.0') and len(toolset_version) > len('14.0'):
        new_toolset_version = '14.0'
        debug(
            'rewrite toolset_version=%s => toolset_version=%s',
            repr(toolset_version), repr(new_toolset_version)
        )
        toolset_version = new_toolset_version

    toolset_vcvars = _msvc_version_toolset_vcvars(msvc, vc_dir, toolset_version)
    debug(
        'toolset: toolset_version=%s, toolset_vcvars=%s',
        repr(toolset_version), repr(toolset_vcvars)
    )

    if not toolset_vcvars:
        err_msg = "MSVC_TOOLSET_VERSION {} not found for MSVC_VERSION {}".format(
            repr(toolset_version), repr(msvc.version)
        )
        raise MSVCToolsetVersionNotFound(err_msg)

    return toolset_vcvars

def _msvc_script_argument_toolset(env, msvc, vc_dir, arglist):

    toolset_version = env['MSVC_TOOLSET_VERSION']
    debug('MSVC_VERSION=%s, MSVC_TOOLSET_VERSION=%s', repr(msvc.version), repr(toolset_version))

    if not toolset_version:
        return None

    toolset_vcvars = _msvc_script_argument_toolset_vcvars(msvc, toolset_version, vc_dir)

    # toolset may not be installed for host/target
    argpair = (SortOrder.TOOLSET, f'-vcvars_ver={toolset_vcvars}')
    arglist.append(argpair)

    return toolset_vcvars

def _msvc_script_default_toolset(env, msvc, vc_dir, arglist, force_toolset: bool=False):

    if msvc.vs_def.vc_buildtools_def.msvc_version_numeric < VS2017.vc_buildtools_def.msvc_version_numeric:
        return None

    toolset_default = _msvc_default_toolset(msvc, vc_dir)
    if not toolset_default:
        return None

    debug('MSVC_VERSION=%s, toolset_default=%s', repr(msvc.version), repr(toolset_default))

    if force_toolset:
        argpair = (SortOrder.TOOLSET, f'-vcvars_ver={toolset_default}')
        arglist.append(argpair)

    return toolset_default

def _user_script_argument_toolset(env, toolset_version, user_argstr):

    matches = [m for m in re_vcvars_toolset.finditer(user_argstr)]
    if not matches:
        return None

    if len(matches) > 1:
        debug('multiple toolset version declarations: MSVC_SCRIPT_ARGS=%s', repr(user_argstr))
        err_msg = f"multiple toolset version declarations: MSVC_SCRIPT_ARGS={user_argstr!r}"
        raise MSVCArgumentError(err_msg)

    if not toolset_version:
        user_toolset = matches[0].group('toolset')
        return user_toolset

    env_argstr = env.get('MSVC_TOOLSET_VERSION','')
    debug('multiple toolset version declarations: MSVC_TOOLSET_VERSION=%s, MSVC_SCRIPT_ARGS=%s', repr(env_argstr), repr(user_argstr))

    err_msg = "multiple toolset version declarations: MSVC_TOOLSET_VERSION={} and MSVC_SCRIPT_ARGS={}".format(
        repr(env_argstr), repr(user_argstr)
    )

    raise MSVCArgumentError(err_msg)

def _msvc_script_argument_spectre_constraints(msvc, toolset, spectre_libs, platform_def):

    if msvc.vs_def.vc_buildtools_def.msvc_version_numeric < VS2017.vc_buildtools_def.msvc_version_numeric:
        debug(
            'invalid: msvc version constraint: %s < %s VS2017',
            repr(msvc.vs_def.vc_buildtools_def.msvc_version_numeric),
            repr(VS2017.vc_buildtools_def.msvc_version_numeric)
        )
        err_msg = "MSVC_SPECTRE_LIBS ({}) constraint violation: MSVC_VERSION {} < {} VS2017".format(
            repr(spectre_libs), repr(msvc.version), repr(VS2017.vc_buildtools_def.msvc_version)
        )
        return err_msg

    if toolset:
        if toolset.vc_buildtools_def.msvc_version_numeric < VS2017.vc_buildtools_def.msvc_version_numeric:
            debug(
                'invalid: toolset version constraint: %s < %s VS2017',
                repr(toolset.vc_buildtools_def.msvc_version_numeric),
                repr(VS2017.vc_buildtools_def.msvc_version_numeric)
            )
            err_msg = "MSVC_SPECTRE_LIBS ({}) constraint violation: toolset version {} < {} VS2017".format(
                repr(spectre_libs), repr(toolset.version), repr(VS2017.vc_buildtools_def.msvc_version)
            )
            return err_msg


    if platform_def.is_uwp:
        debug(
            'invalid: spectre_libs=%s and platform_type=%s',
            repr(spectre_libs), repr(platform_def.vc_platform)
        )
        err_msg = "MSVC_SPECTRE_LIBS ({}) are not supported for platform type ({})".format(
            repr(spectre_libs), repr(platform_def.vc_platform)
        )
        return err_msg

    return None

def _msvc_toolset_version_spectre_path(vc_dir, toolset_version):
    spectre_dir = os.path.join(vc_dir, "Tools", "MSVC", toolset_version, "lib", "spectre")
    return spectre_dir

def _msvc_script_argument_spectre(env, msvc, vc_dir, toolset, platform_def, arglist):

    spectre_libs = env['MSVC_SPECTRE_LIBS']
    debug('MSVC_VERSION=%s, MSVC_SPECTRE_LIBS=%s', repr(msvc.version), repr(spectre_libs))

    if not spectre_libs:
        return None

    if spectre_libs not in _ARGUMENT_BOOLEAN_TRUE:
        return None

    err_msg = _msvc_script_argument_spectre_constraints(msvc, toolset, spectre_libs, platform_def)
    if err_msg:
        raise MSVCArgumentError(err_msg)

    if toolset:
        spectre_dir = _msvc_toolset_version_spectre_path(vc_dir, toolset.version)
        if not os.path.exists(spectre_dir):
            debug(
                'spectre libs: msvc_version=%s, toolset_version=%s, spectre_dir=%s',
                repr(msvc.version), repr(toolset.version), repr(spectre_dir)
            )
            err_msg = "Spectre libraries not found for MSVC_VERSION {} toolset version {}".format(
                repr(msvc.version), repr(toolset.version)
            )
            raise MSVCSpectreLibsNotFound(err_msg)

    spectre_arg = 'spectre'

    # spectre libs may not be installed for host/target
    argpair = (SortOrder.SPECTRE, f'-vcvars_spectre_libs={spectre_arg}')
    arglist.append(argpair)

    return spectre_arg

def _user_script_argument_spectre(env, spectre, user_argstr):

    matches = [m for m in re_vcvars_spectre.finditer(user_argstr)]
    if not matches:
        return None

    if len(matches) > 1:
        debug('multiple spectre declarations: MSVC_SCRIPT_ARGS=%s', repr(user_argstr))
        err_msg = f"multiple spectre declarations: MSVC_SCRIPT_ARGS={user_argstr!r}"
        raise MSVCArgumentError(err_msg)

    if not spectre:
        return None

    env_argstr = env.get('MSVC_SPECTRE_LIBS','')
    debug('multiple spectre declarations: MSVC_SPECTRE_LIBS=%s, MSVC_SCRIPT_ARGS=%s', repr(env_argstr), repr(user_argstr))

    err_msg = "multiple spectre declarations: MSVC_SPECTRE_LIBS={} and MSVC_SCRIPT_ARGS={}".format(
        repr(env_argstr), repr(user_argstr)
    )

    raise MSVCArgumentError(err_msg)

def _msvc_script_argument_user(env, msvc, arglist):

    # subst None -> empty string
    script_args = env.subst('$MSVC_SCRIPT_ARGS')
    debug('MSVC_VERSION=%s, MSVC_SCRIPT_ARGS=%s', repr(msvc.version), repr(script_args))

    if not script_args:
        return None

    if msvc.vs_def.vc_buildtools_def.msvc_version_numeric < VS2015.vc_buildtools_def.msvc_version_numeric:
        debug(
            'invalid: msvc version constraint: %s < %s VS2015',
            repr(msvc.vs_def.vc_buildtools_def.msvc_version_numeric),
            repr(VS2015.vc_buildtools_def.msvc_version_numeric)
        )
        err_msg = "MSVC_SCRIPT_ARGS ({}) constraint violation: MSVC_VERSION {} < {} VS2015".format(
            repr(script_args), repr(msvc.version), repr(VS2015.vc_buildtools_def.msvc_version)
        )
        raise MSVCArgumentError(err_msg)

    # user arguments are not validated
    argpair = (SortOrder.USER, script_args)
    arglist.append(argpair)

    return script_args

def _msvc_process_construction_variables(env) -> bool:

    for cache_variable in [
        _MSVC_FORCE_DEFAULT_TOOLSET,
        _MSVC_FORCE_DEFAULT_SDK,
    ]:
        if cache_variable:
            return True

    for env_variable in [
        'MSVC_UWP_APP',
        'MSVC_TOOLSET_VERSION',
        'MSVC_SDK_VERSION',
        'MSVC_SPECTRE_LIBS',
    ]:
        if env.get(env_variable, None) is not None:
            return True

    return False

def msvc_script_arguments_has_uwp(env):

    if not _msvc_process_construction_variables(env):
        return False

    uwp_app = env.get('MSVC_UWP_APP')
    is_uwp = bool(uwp_app and uwp_app in _ARGUMENT_BOOLEAN_TRUE_LEGACY)

    debug('is_uwp=%s', is_uwp)
    return is_uwp

def msvc_script_arguments(env, version, vc_dir, arg=None):

    arguments = [arg] if arg else []

    arglist = []
    arglist_reverse = False

    msvc = _msvc_version(version)

    if 'MSVC_SCRIPT_ARGS' in env:
        user_argstr = _msvc_script_argument_user(env, msvc, arglist)
    else:
        user_argstr = None

    if _msvc_process_construction_variables(env):

        target_arch = env.get('TARGET_ARCH')

        # MSVC_UWP_APP

        if 'MSVC_UWP_APP' in env:
            uwp = _msvc_script_argument_uwp(env, msvc, arglist, target_arch)
        else:
            uwp = None

        if user_argstr:
            user_uwp = _user_script_argument_uwp(env, uwp, user_argstr)
        else:
            user_uwp = None

        is_uwp = True if uwp else False
        platform_def = WinSDK.get_msvc_platform(is_uwp)

        # MSVC_TOOLSET_VERSION

        if 'MSVC_TOOLSET_VERSION' in env:
            toolset_version = _msvc_script_argument_toolset(env, msvc, vc_dir, arglist)
        else:
            toolset_version = None

        if user_argstr:
            user_toolset = _user_script_argument_toolset(env, toolset_version, user_argstr)
        else:
            user_toolset = None

        if not toolset_version and not user_toolset:
            default_toolset = _msvc_script_default_toolset(env, msvc, vc_dir, arglist, _MSVC_FORCE_DEFAULT_TOOLSET)
            if _MSVC_FORCE_DEFAULT_TOOLSET:
                toolset_version = default_toolset
        else:
            default_toolset = None

        if user_toolset:
            toolset = None
        elif toolset_version:
            toolset = _toolset_version(toolset_version, is_user=True)
        elif default_toolset:
            toolset = _toolset_version(default_toolset)
        else:
            toolset = None

        # MSVC_SDK_VERSION

        if 'MSVC_SDK_VERSION' in env:
            sdk_version = _msvc_script_argument_sdk(env, msvc, toolset, platform_def, arglist)
        else:
            sdk_version = None

        if user_argstr:
            user_sdk = _user_script_argument_sdk(env, sdk_version, user_argstr)
        else:
            user_sdk = None

        if _MSVC_FORCE_DEFAULT_SDK:
            if not sdk_version and not user_sdk:
                sdk_version = _msvc_script_default_sdk(env, msvc, platform_def, arglist, _MSVC_FORCE_DEFAULT_SDK)

        # MSVC_SPECTRE_LIBS

        if 'MSVC_SPECTRE_LIBS' in env:
            spectre = _msvc_script_argument_spectre(env, msvc, vc_dir, toolset, platform_def, arglist)
        else:
            spectre = None

        if user_argstr:
            _user_script_argument_spectre(env, spectre, user_argstr)

        if msvc.vs_def.vc_buildtools_def.msvc_version == '14.0':
            if user_uwp and sdk_version and len(arglist) == 2:
                # VS2015 toolset argument order issue: SDK store => store SDK
                arglist_reverse = True

    if len(arglist) > 1:
        arglist.sort()
        if arglist_reverse:
            arglist.reverse()

    arguments.extend([argpair[-1] for argpair in arglist])
    argstr = ' '.join(arguments).strip()

    debug('arguments: %s', repr(argstr))
    return argstr

def _msvc_toolset_internal(msvc_version, toolset_version, vc_dir):

    msvc = _msvc_version(msvc_version)

    toolset_vcvars = _msvc_script_argument_toolset_vcvars(msvc, toolset_version, vc_dir)

    return toolset_vcvars

def _msvc_toolset_versions_internal(msvc_version, vc_dir, full: bool=True, sxs: bool=False):

    msvc = _msvc_version(msvc_version)

    if len(msvc.vs_def.vc_buildtools_all) <= 1:
        return None

    toolset_versions = []

    toolsets_sxs, toolsets_full = _msvc_version_toolsets(msvc, vc_dir)

    if sxs:
        sxs_versions = list(toolsets_sxs.keys())
        sxs_versions.sort(reverse=True)
        toolset_versions.extend(sxs_versions)

    if full:
        toolset_versions.extend(toolsets_full)

    return toolset_versions

def _msvc_version_toolsets_internal(msvc_version, vc_dir):

    msvc = _msvc_version(msvc_version)

    toolsets_sxs, toolsets_full = _msvc_version_toolsets(msvc, vc_dir)

    return toolsets_sxs, toolsets_full

def _msvc_toolset_versions_spectre_internal(msvc_version, vc_dir):

    msvc = _msvc_version(msvc_version)

    if len(msvc.vs_def.vc_buildtools_all) <= 1:
        return None

    _, toolsets_full = _msvc_version_toolsets(msvc, vc_dir)

    spectre_toolset_versions = [
        toolset_version
        for toolset_version in toolsets_full
        if os.path.exists(_msvc_toolset_version_spectre_path(vc_dir, toolset_version))
    ]

    return spectre_toolset_versions

def reset() -> None:
    debug('')
    _reset_have140_cache()
    _reset_toolset_cache()

def verify() -> None:
    debug('')
    _verify_re_sdk_dispatch_map()

