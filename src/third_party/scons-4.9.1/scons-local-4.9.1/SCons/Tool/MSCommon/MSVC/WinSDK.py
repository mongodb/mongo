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
Windows SDK functions for Microsoft Visual C/C++.
"""

import os

from ..common import (
    debug,
)

from . import Util
from . import Config
from . import Registry

from .Exceptions import (
    MSVCInternalError,
)

from . import Dispatcher
Dispatcher.register_modulename(__name__)


_DESKTOP = Config.MSVC_PLATFORM_INTERNAL['Desktop']
_UWP = Config.MSVC_PLATFORM_INTERNAL['UWP']

def _new_sdk_map():
    sdk_map = {
        _DESKTOP.vc_platform: [],
        _UWP.vc_platform: [],
    }
    return sdk_map

def _sdk_10_layout(version):

    folder_prefix = version + '.'

    sdk_map = _new_sdk_map()

    sdk_roots = Registry.sdk_query_paths(version)

    sdk_version_platform_seen = set()
    sdk_roots_seen = set()

    for sdk_t in sdk_roots:

        sdk_root = sdk_t[0]
        if sdk_root in sdk_roots_seen:
            continue
        sdk_roots_seen.add(sdk_root)

        if not os.path.exists(sdk_root):
            continue

        sdk_include_path = os.path.join(sdk_root, 'include')
        if not os.path.exists(sdk_include_path):
            continue

        for version_nbr, version_nbr_path in Util.listdir_dirs(sdk_include_path):

            if not version_nbr.startswith(folder_prefix):
                continue

            sdk_inc_path = Util.normalize_path(os.path.join(version_nbr_path, 'um'))
            if not os.path.exists(sdk_inc_path):
                continue

            for vc_platform, sdk_inc_file in [
                (_DESKTOP.vc_platform, 'winsdkver.h'),
                (_UWP.vc_platform,     'windows.h'),
            ]:

                if not os.path.exists(os.path.join(sdk_inc_path, sdk_inc_file)):
                    continue

                key = (version_nbr, vc_platform)
                if key in sdk_version_platform_seen:
                    continue
                sdk_version_platform_seen.add(key)

                sdk_map[vc_platform].append(version_nbr)

    for key, val in sdk_map.items():
        val.sort(reverse=True)

    return sdk_map

def _sdk_81_layout(version):

    version_nbr = version

    sdk_map = _new_sdk_map()

    sdk_roots = Registry.sdk_query_paths(version)

    sdk_version_platform_seen = set()
    sdk_roots_seen = set()

    for sdk_t in sdk_roots:

        sdk_root = sdk_t[0]
        if sdk_root in sdk_roots_seen:
            continue
        sdk_roots_seen.add(sdk_root)

        # msvc does not check for existence of root or other files

        sdk_inc_path = Util.normalize_path(os.path.join(sdk_root, r'include\um'))
        if not os.path.exists(sdk_inc_path):
            continue

        for vc_platform, sdk_inc_file in [
            (_DESKTOP.vc_platform, 'winsdkver.h'),
            (_UWP.vc_platform,     'windows.h'),
        ]:

            if not os.path.exists(os.path.join(sdk_inc_path, sdk_inc_file)):
                continue

            key = (version_nbr, vc_platform)
            if key in sdk_version_platform_seen:
                continue
            sdk_version_platform_seen.add(key)

            sdk_map[vc_platform].append(version_nbr)

    for key, val in sdk_map.items():
        val.sort(reverse=True)

    return sdk_map

_sdk_map_cache = {}
_sdk_cache = {}

def _reset_sdk_cache() -> None:
    global _sdk_map_cache
    global _sdk_cache
    debug('')
    _sdk_map_cache = {}
    _sdk_cache = {}

def _sdk_10(key, reg_version):
    if key in _sdk_map_cache:
        sdk_map = _sdk_map_cache[key]
    else:
        sdk_map = _sdk_10_layout(reg_version)
        _sdk_map_cache[key] = sdk_map
    return sdk_map

def _sdk_81(key, reg_version):
    if key in _sdk_map_cache:
        sdk_map = _sdk_map_cache[key]
    else:
        sdk_map = _sdk_81_layout(reg_version)
        _sdk_map_cache[key] = sdk_map
    return sdk_map

def _combine_sdk_map_list(sdk_map_list):
    combined_sdk_map = _new_sdk_map()
    for sdk_map in sdk_map_list:
        for key, val in sdk_map.items():
            combined_sdk_map[key].extend(val)
    return combined_sdk_map

_sdk_dispatch_map = {
    '10.0': (_sdk_10, '10.0'),
    '8.1': (_sdk_81, '8.1'),
}

def _verify_sdk_dispatch_map():
    debug('')
    for sdk_version in Config.MSVC_SDK_VERSIONS:
        if sdk_version in _sdk_dispatch_map:
            continue
        err_msg = f'sdk version {sdk_version} not in sdk_dispatch_map'
        raise MSVCInternalError(err_msg)
    return None

def _version_list_sdk_map(version_list):
    sdk_map_list = []
    for version in version_list:
        func, reg_version = _sdk_dispatch_map[version]
        sdk_map = func(version, reg_version)
        sdk_map_list.append(sdk_map)

    combined_sdk_map = _combine_sdk_map_list(sdk_map_list)
    return combined_sdk_map

def _sdk_map(version_list):
    key = tuple(version_list)
    if key in _sdk_cache:
        sdk_map = _sdk_cache[key]
    else:
        version_numlist = [float(v) for v in version_list]
        version_numlist.sort(reverse=True)
        key = tuple([str(v) for v in version_numlist])
        sdk_map = _version_list_sdk_map(key)
        _sdk_cache[key] = sdk_map
    return sdk_map

def get_msvc_platform(is_uwp: bool=False):
    platform_def = _UWP if is_uwp else _DESKTOP
    return platform_def

def get_sdk_version_list(vs_def, platform_def):
    version_list = vs_def.vc_sdk_versions if vs_def.vc_sdk_versions is not None else []
    sdk_map = _sdk_map(version_list)
    sdk_list = sdk_map.get(platform_def.vc_platform, [])
    return sdk_list

def get_msvc_sdk_version_list(msvc_version, msvc_uwp_app: bool=False):
    debug('msvc_version=%s, msvc_uwp_app=%s', repr(msvc_version), repr(msvc_uwp_app))

    sdk_versions = []

    verstr = Util.get_msvc_version_prefix(msvc_version)
    if not verstr:
        debug('msvc_version is not defined')
        return sdk_versions

    vs_def = Config.MSVC_VERSION_EXTERNAL.get(verstr, None)
    if not vs_def:
        debug('vs_def is not defined')
        return sdk_versions

    is_uwp = True if msvc_uwp_app in Config.BOOLEAN_SYMBOLS[True] else False
    platform_def = get_msvc_platform(is_uwp)
    sdk_list = get_sdk_version_list(vs_def, platform_def)

    sdk_versions.extend(sdk_list)
    debug('sdk_versions=%s', repr(sdk_versions))

    return sdk_versions

def reset() -> None:
    debug('')
    _reset_sdk_cache()

def verify() -> None:
    debug('')
    _verify_sdk_dispatch_map()

