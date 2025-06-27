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
Helper functions for Microsoft Visual C/C++.
"""

import os
import pathlib
import re

from collections import (
    namedtuple,
)

from ..common import debug

from . import Config


# call _initialize method upon class definition completion

class AutoInitialize:
    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        if hasattr(cls, '_initialize') and callable(getattr(cls, '_initialize', None)):
            cls._initialize()

# path utilities

# windows drive specification (e.g., 'C:')
_RE_DRIVESPEC = re.compile(r'^[A-Za-z][:]$', re.IGNORECASE)

# windows path separators
_OS_PATH_SEPS = (os.path.sep, os.path.altsep) if os.path.altsep else (os.path.sep,)

def listdir_dirs(p):
    """
    Return a list of tuples for each subdirectory of the given directory path.
    Each tuple is comprised of the subdirectory name and the qualified subdirectory path.

    Args:
        p: str
            directory path

    Returns:
        list[tuple[str,str]]: a list of tuples

    """
    dirs = []
    if p and os.path.exists(p) and os.path.isdir(p):
        for dir_name in os.listdir(p):
            dir_path = os.path.join(p, dir_name)
            if os.path.isdir(dir_path):
                dirs.append((dir_name, dir_path))
    return dirs

def resolve_path(p, ignore_drivespec=True):
    """
    Make path absolute resolving any symlinks

    Args:
        p: str
            system path
        ignore_drivespec: bool
            ignore drive specifications when True

    Returns:
        str: absolute path with symlinks resolved

    """

    if p:

        if ignore_drivespec and _RE_DRIVESPEC.match(p):
            # don't attempt to resolve drive specification (e.g., C:)
            pass
        else:
            # both abspath and resolve necessary for an unqualified file name
            # on a mapped network drive in order to return a mapped drive letter
            # path rather than a UNC path.
            p = os.path.abspath(p)
            try:
                p = str(pathlib.Path(p).resolve())
            except OSError as e:
                debug(
                    'caught exception: path=%s, exception=%s(%s)',
                    repr(p), type(e).__name__, repr(str(e))
                )

    return p

def normalize_path(
    p,
    strip=True,
    preserve_trailing=False,
    expand=False,
    realpath=True,
    ignore_drivespec=True,
):
    """
    Normalize path

    Args:
        p: str
            system path
        strip: bool
            remove leading and trailing whitespace when True
        preserve_trailing: bool
            preserve trailing path separator when True
        expand: bool
            apply expanduser and expandvars when True
        realpath: bool
            make the path absolute resolving any symlinks when True
        ignore_drivespec: bool
            ignore drive specifications for realpath when True

    Returns:
        str: normalized path

    """

    if p and strip:
        p = p.strip()

    if p:

        trailing = bool(preserve_trailing and p.endswith(_OS_PATH_SEPS))

        if expand:
            p = os.path.expanduser(p)
            p = os.path.expandvars(p)

        p = os.path.normpath(p)

        if realpath:
            p = resolve_path(p, ignore_drivespec=ignore_drivespec)

        p = os.path.normcase(p)

        if trailing:
            p += os.path.sep

    return p

# msvc version and msvc toolset version regexes

re_version_prefix = re.compile('^(?P<version>[0-9]+(?:[.][0-9]+)*)(?![.]).*$')

re_msvc_version_prefix = re.compile(r'^(?P<version>[1-9][0-9]?[.][0-9]).*$')

re_msvc_version = re.compile(r'^(?P<msvc_version>[1-9][0-9]?[.][0-9])(?P<suffix>[A-Z]+)*$', re.IGNORECASE)

re_extended_version = re.compile(r'''^
    (?P<version>(?:
        ([1-9][0-9]?[.][0-9]{1,2})|                     # XX.Y       - XX.YY
        ([1-9][0-9][.][0-9]{2}[.][0-9]{1,5})|           # XX.YY.Z    - XX.YY.ZZZZZ
        ([1-9][0-9][.][0-9]{2}[.][0-9]{2}[.][0-9]{1,2}) # XX.YY.AA.B - XX.YY.AA.BB
    ))
    (?P<suffix>[A-Z]+)*
$''', re.IGNORECASE | re.VERBOSE)

re_toolset_full = re.compile(r'''^(?:
    (?:[1-9][0-9][.][0-9]{1,2})|           # XX.Y    - XX.YY
    (?:[1-9][0-9][.][0-9]{2}[.][0-9]{1,5}) # XX.YY.Z - XX.YY.ZZZZZ
)$''', re.VERBOSE)

re_toolset_140 = re.compile(r'''^(?:
    (?:14[.]0{1,2})|       # 14.0    - 14.00
    (?:14[.]0{2}[.]0{1,5}) # 14.00.0 - 14.00.00000
)$''', re.VERBOSE)

re_toolset_sxs = re.compile(
    r'^[1-9][0-9][.][0-9]{2}[.][0-9]{2}[.][0-9]{1,2}$' # MM.mm.VV.vv format
)

# msvc sdk version regexes

re_msvc_sdk_version = re.compile(r'''^
    (?P<version>(?:
        ([1-9][0-9]?[.][0-9])|                           # XX.Y
        ([1-9][0-9][.][0-9]{1}[.][0-9]{5}[.][0-9]{1,2})  # XX.Y.ZZZZZ.A - XX.Y.ZZZZZ.AA
    ))
$''', re.IGNORECASE | re.VERBOSE)

# version prefix utilities

def get_version_prefix(version):
    """
    Get the version number prefix from a string.

    Args:
        version: str
            version specification

    Returns:
        str: the version number prefix

    """
    rval = ''
    if version:
        m = re_version_prefix.match(version)
        if m:
            rval = m.group('version')
    return rval

def get_msvc_version_prefix(version):
    """
    Get the msvc version number prefix from a string.

    Args:
        version: str
            version specification

    Returns:
        str: the msvc version number prefix

    """
    rval = ''
    if version:
        m = re_msvc_version_prefix.match(version)
        if m:
            rval = m.group('version')
    return rval

def get_msvc_version_prefix_suffix(version):
    """
    Get the msvc version number prefix and suffix from a string.

    Args:
        version: str
            version specification

    Returns:
        (str, str): the msvc version prefix and suffix

    """
    prefix = suffix = ''
    if version:
        m = re_msvc_version.match(version)
        if m:
            prefix = m.group('msvc_version')
            suffix = m.group('suffix') if m.group('suffix') else ''
    return prefix, suffix

# toolset version query utilities

def is_toolset_full(toolset_version) -> bool:
    rval = False
    if toolset_version:
        if re_toolset_full.match(toolset_version):
            rval = True
    return rval

def is_toolset_140(toolset_version) -> bool:
    rval = False
    if toolset_version:
        if re_toolset_140.match(toolset_version):
            rval = True
    return rval

def is_toolset_sxs(toolset_version) -> bool:
    rval = False
    if toolset_version:
        if re_toolset_sxs.match(toolset_version):
            rval = True
    return rval

# msvc version and msvc toolset version decomposition utilties

_MSVC_VERSION_COMPONENTS_DEFINITION = namedtuple('MSVCVersionComponentsDefinition', [
    'msvc_version', # msvc version (e.g., '14.1Exp')
    'msvc_verstr',  # msvc version numeric string (e.g., '14.1')
    'msvc_suffix',  # msvc version component type (e.g., 'Exp')
    'msvc_vernum',  # msvc version floating point number (e.g, 14.1)
    'msvc_major',   # msvc major version integer number (e.g., 14)
    'msvc_minor',   # msvc minor version integer number (e.g., 1)
    'msvc_comps',   # msvc version components tuple (e.g., ('14', '1'))
])

def msvc_version_components(vcver):
    """
    Decompose an msvc version into components.

    Tuple fields:
        msvc_version: msvc version (e.g., '14.1Exp')
        msvc_verstr:  msvc version numeric string (e.g., '14.1')
        msvc_suffix:  msvc version component type (e.g., 'Exp')
        msvc_vernum:  msvc version floating point number (e.g., 14.1)
        msvc_major:   msvc major version integer number (e.g., 14)
        msvc_minor:   msvc minor version integer number (e.g., 1)
        msvc_comps:   msvc version components tuple (e.g., ('14', '1'))

    Args:
        vcver: str
            msvc version specification

    Returns:
        None or MSVCVersionComponents namedtuple:
    """

    if not vcver:
        return None

    m = re_msvc_version.match(vcver)
    if not m:
        return None

    vs_def = Config.MSVC_VERSION_SUFFIX.get(vcver)
    if not vs_def:
        return None

    msvc_version = vcver
    msvc_verstr = m.group('msvc_version')
    msvc_suffix = m.group('suffix') if m.group('suffix') else ''
    msvc_vernum = float(msvc_verstr)

    msvc_comps = tuple(msvc_verstr.split('.'))
    msvc_major, msvc_minor = (int(x) for x in msvc_comps)

    msvc_version_components_def = _MSVC_VERSION_COMPONENTS_DEFINITION(
        msvc_version = msvc_version,
        msvc_verstr = msvc_verstr,
        msvc_suffix = msvc_suffix,
        msvc_vernum = msvc_vernum,
        msvc_major = msvc_major,
        msvc_minor = msvc_minor,
        msvc_comps = msvc_comps,
    )

    return msvc_version_components_def

_MSVC_EXTENDED_VERSION_COMPONENTS_DEFINITION = namedtuple('MSVCExtendedVersionComponentsDefinition', [
    'msvc_version',      # msvc version (e.g., '14.1Exp')
    'msvc_verstr',       # msvc version numeric string (e.g., '14.1')
    'msvc_suffix',       # msvc version component type (e.g., 'Exp')
    'msvc_suffix_rank',  # msvc version component rank (0, 1)
    'msvc_vernum',       # msvc version floating point number (e.g, 14.1)
    'msvc_major',        # msvc major version integer number (e.g., 14)
    'msvc_minor',        # msvc minor version integer number (e.g., 1)
    'msvc_comps',        # msvc version components tuple (e.g., ('14', '1'))
    'msvc_buildtools',       # msvc build tools
    'msvc_buildtools_num',   # msvc build tools integer number
    'msvc_buildseries',      # msvc build series
    'msvc_buildseries_num',  # msvc build series floating point number
    'msvc_toolset_version',  # msvc toolset version
    'msvc_toolset_comps',    # msvc toolset version components
    'msvc_toolset_is_sxs',   # msvc toolset version is sxs
    'version',               # msvc version or msvc toolset version
])

def msvc_extended_version_components(version):
    """
    Decompose an msvc version or msvc toolset version into components.

    Args:
        version: str
            version specification

    Returns:
        None or MSVCExtendedVersionComponents namedtuple:
    """

    if not version:
        return None

    m = re_extended_version.match(version)
    if not m:
        return None

    msvc_toolset_version = m.group('version')
    msvc_toolset_comps = tuple(msvc_toolset_version.split('.'))
    msvc_toolset_is_sxs = is_toolset_sxs(msvc_toolset_version)

    vc_verstr = get_msvc_version_prefix(msvc_toolset_version)
    if not vc_verstr:
        return None

    vc_buildseries_def = Config.MSVC_BUILDSERIES_EXTERNAL.get(vc_verstr)
    if not vc_buildseries_def:
        return None

    vc_buildtools_def = Config.VC_BUILDTOOLS_MAP[vc_buildseries_def.vc_buildseries]

    msvc_verstr = vc_buildtools_def.msvc_version
    msvc_suffix = m.group('suffix') if m.group('suffix') else ''
    msvc_version = msvc_verstr + msvc_suffix

    vs_def = Config.MSVC_VERSION_SUFFIX.get(msvc_version)
    if not vs_def:
        return None

    msvc_vernum = float(msvc_verstr)

    msvc_comps = tuple(msvc_verstr.split('.'))
    msvc_major, msvc_minor = (int(x) for x in msvc_comps)

    msvc_extended_version_components_def = _MSVC_EXTENDED_VERSION_COMPONENTS_DEFINITION(
        msvc_version = msvc_version,
        msvc_verstr = msvc_verstr,
        msvc_suffix = msvc_suffix,
        msvc_suffix_rank = 0 if not msvc_suffix else 1,
        msvc_vernum = msvc_vernum,
        msvc_major = msvc_major,
        msvc_minor = msvc_minor,
        msvc_comps = msvc_comps,
        msvc_buildtools = vc_buildtools_def.msvc_version,
        msvc_buildtools_num = vc_buildtools_def.msvc_version_numeric,
        msvc_buildseries = vc_buildseries_def.vc_version,
        msvc_buildseries_num = vc_buildseries_def.vc_version_numeric,
        msvc_toolset_version = msvc_toolset_version,
        msvc_toolset_comps = msvc_toolset_comps,
        msvc_toolset_is_sxs = msvc_toolset_is_sxs,
        version = version,
    )

    return msvc_extended_version_components_def

# msvc sdk version decomposition utilties

_MSVC_SDK_VERSION_COMPONENTS_DEFINITION = namedtuple('MSVCSDKVersionComponentsDefinition', [
    'sdk_version', # sdk version (e.g., '10.0.20348.0')
    'sdk_verstr',  # sdk version numeric string (e.g., '10.0')
    'sdk_vernum',  # sdk version floating point number (e.g, 10.0)
    'sdk_major',   # sdk major version integer number (e.g., 10)
    'sdk_minor',   # sdk minor version integer number (e.g., 0)
    'sdk_comps',   # sdk version components tuple (e.g., ('10', '0', '20348', '0'))
])

def msvc_sdk_version_components(version):
    """
    Decompose an msvc sdk version into components.

    Tuple fields:
        sdk_version: sdk version (e.g., '10.0.20348.0')
        sdk_verstr:  sdk version numeric string (e.g., '10.0')
        sdk_vernum:  sdk version floating point number (e.g., 10.0)
        sdk_major:   sdk major version integer number (e.g., 10)
        sdk_minor:   sdk minor version integer number (e.g., 0)
        sdk_comps:   sdk version components tuple (e.g., ('10', '0', '20348', '0'))

    Args:
        version: str
            sdk version specification

    Returns:
        None or MSVCSDKVersionComponents namedtuple:
    """

    if not version:
        return None

    m = re_msvc_sdk_version.match(version)
    if not m:
        return None

    sdk_version = version
    sdk_comps = tuple(sdk_version.split('.'))
    sdk_verstr = '.'.join(sdk_comps[:2])
    sdk_vernum = float(sdk_verstr)

    sdk_major, sdk_minor = (int(x) for x in sdk_comps[:2])

    msvc_sdk_version_components_def = _MSVC_SDK_VERSION_COMPONENTS_DEFINITION(
        sdk_version = sdk_version,
        sdk_verstr = sdk_verstr,
        sdk_vernum = sdk_vernum,
        sdk_major = sdk_major,
        sdk_minor = sdk_minor,
        sdk_comps = sdk_comps,
    )

    return msvc_sdk_version_components_def

