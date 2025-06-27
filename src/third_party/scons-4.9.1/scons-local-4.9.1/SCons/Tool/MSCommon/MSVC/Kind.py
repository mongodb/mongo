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
Version kind categorization for Microsoft Visual C/C++.
"""

import os
import re

from collections import (
    namedtuple,
)

from ..common import (
    debug,
)

from . import Registry
from . import Util

from . import Dispatcher
Dispatcher.register_modulename(__name__)


# use express install for non-express msvc_version if no other installations found
USE_EXPRESS_FOR_NONEXPRESS = True

# productdir kind

VCVER_KIND_UNKNOWN = 0  # undefined
VCVER_KIND_DEVELOP = 1  # devenv binary
VCVER_KIND_EXPRESS = 2  # express binary
VCVER_KIND_BTDISPATCH = 3  # no ide binaries (buildtools dispatch folder)
VCVER_KIND_VCFORPYTHON = 4  # no ide binaries (2008/9.0)
VCVER_KIND_EXPRESS_WIN = 5  # express for windows binary (VSWinExpress)
VCVER_KIND_EXPRESS_WEB = 6  # express for web binary (VWDExpress)
VCVER_KIND_SDK = 7  # no ide binaries
VCVER_KIND_CMDLINE = 8  # no ide binaries

VCVER_KIND_STR = {
    VCVER_KIND_UNKNOWN: '<Unknown>',
    VCVER_KIND_DEVELOP: 'Develop',
    VCVER_KIND_EXPRESS: 'Express',
    VCVER_KIND_BTDISPATCH: 'BTDispatch',
    VCVER_KIND_VCFORPYTHON: 'VCForPython',
    VCVER_KIND_EXPRESS_WIN: 'Express-Win',
    VCVER_KIND_EXPRESS_WEB: 'Express-Web',
    VCVER_KIND_SDK: 'SDK',
    VCVER_KIND_CMDLINE: 'CmdLine',
}

BITFIELD_KIND_DEVELOP     = 0b_1000
BITFIELD_KIND_EXPRESS     = 0b_0100
BITFIELD_KIND_EXPRESS_WIN = 0b_0010
BITFIELD_KIND_EXPRESS_WEB = 0b_0001

VCVER_KIND_PROGRAM = namedtuple("VCVerKindProgram", [
    'kind',  # relpath from pdir to vsroot
    'program',  # ide binaries
    'bitfield',
])

#

IDE_PROGRAM_DEVENV_COM = VCVER_KIND_PROGRAM(
    kind=VCVER_KIND_DEVELOP,
    program='devenv.com',
    bitfield=BITFIELD_KIND_DEVELOP,
)

IDE_PROGRAM_MSDEV_COM = VCVER_KIND_PROGRAM(
    kind=VCVER_KIND_DEVELOP,
    program='msdev.com',
    bitfield=BITFIELD_KIND_DEVELOP,
)

IDE_PROGRAM_WDEXPRESS_EXE = VCVER_KIND_PROGRAM(
    kind=VCVER_KIND_EXPRESS,
    program='WDExpress.exe',
    bitfield=BITFIELD_KIND_EXPRESS,
)

IDE_PROGRAM_VCEXPRESS_EXE = VCVER_KIND_PROGRAM(
    kind=VCVER_KIND_EXPRESS,
    program='VCExpress.exe',
    bitfield=BITFIELD_KIND_EXPRESS,
)

IDE_PROGRAM_VSWINEXPRESS_EXE = VCVER_KIND_PROGRAM(
    kind=VCVER_KIND_EXPRESS_WIN,
    program='VSWinExpress.exe',
    bitfield=BITFIELD_KIND_EXPRESS_WIN,
)

IDE_PROGRAM_VWDEXPRESS_EXE = VCVER_KIND_PROGRAM(
    kind=VCVER_KIND_EXPRESS_WEB,
    program='VWDExpress.exe',
    bitfield=BITFIELD_KIND_EXPRESS_WEB,
)

# detection configuration

VCVER_KIND_DETECT = namedtuple("VCVerKindDetect", [
    'root',  # relpath from pdir to vsroot
    'path',  # vsroot to ide dir
    'programs',  # ide binaries
])

# detected binaries

VCVER_DETECT_BINARIES = namedtuple("VCVerDetectBinaries", [
    'bitfields',  # detect values
    'have_dev',  # develop ide binary
    'have_exp',  # express ide binary
    'have_exp_win',  # express windows ide binary
    'have_exp_web',  # express web ide binary
])


VCVER_DETECT_KIND = namedtuple("VCVerDetectKind", [
    'skip',  # skip vs root
    'save',  # save in case no other kind found
    'kind',  # internal kind
    'binaries_t',
    'extended',
])

# unknown value

_VCVER_DETECT_KIND_UNKNOWN = VCVER_DETECT_KIND(
    skip=True,
    save=False,
    kind=VCVER_KIND_UNKNOWN,
    binaries_t=VCVER_DETECT_BINARIES(
        bitfields=0b0,
        have_dev=False,
        have_exp=False,
        have_exp_win=False,
        have_exp_web=False,
    ),
    extended={},
)

#

_msvc_pdir_func = None

def register_msvc_version_pdir_func(func):
    global _msvc_pdir_func
    if func:
        _msvc_pdir_func = func

_cache_vcver_kind_map = {}

def msvc_version_register_kind(msvc_version, kind_t) -> None:
    global _cache_vcver_kind_map
    if kind_t is None:
        kind_t = _VCVER_DETECT_KIND_UNKNOWN
    debug('msvc_version=%s, kind=%s', repr(msvc_version), repr(VCVER_KIND_STR[kind_t.kind]))
    _cache_vcver_kind_map[msvc_version] = kind_t

def _msvc_version_kind_lookup(msvc_version, env=None):
    global _cache_vcver_kind_map
    global _msvc_pdir_func
    if msvc_version not in _cache_vcver_kind_map:
        _msvc_pdir_func(msvc_version, env)
    kind_t = _cache_vcver_kind_map.get(msvc_version, _VCVER_DETECT_KIND_UNKNOWN)
    debug(
        'kind=%s, dev=%s, exp=%s, msvc_version=%s',
        repr(VCVER_KIND_STR[kind_t.kind]),
        kind_t.binaries_t.have_dev, kind_t.binaries_t.have_exp,
        repr(msvc_version)
    )
    return kind_t

def msvc_version_is_btdispatch(msvc_version, env=None):
    kind_t = _msvc_version_kind_lookup(msvc_version, env)
    is_btdispatch = bool(kind_t.kind == VCVER_KIND_BTDISPATCH)
    debug(
        'is_btdispatch=%s, kind:%s, msvc_version=%s',
        repr(is_btdispatch), repr(VCVER_KIND_STR[kind_t.kind]), repr(msvc_version)
    )
    return is_btdispatch

def msvc_version_is_express(msvc_version, env=None):
    kind_t = _msvc_version_kind_lookup(msvc_version, env)
    is_express = bool(kind_t.kind == VCVER_KIND_EXPRESS)
    debug(
        'is_express=%s, kind:%s, msvc_version=%s',
        repr(is_express), repr(VCVER_KIND_STR[kind_t.kind]), repr(msvc_version)
    )
    return is_express

def msvc_version_is_vcforpython(msvc_version, env=None):
    kind_t = _msvc_version_kind_lookup(msvc_version, env)
    is_vcforpython = bool(kind_t.kind == VCVER_KIND_VCFORPYTHON)
    debug(
        'is_vcforpython=%s, kind:%s, msvc_version=%s',
        repr(is_vcforpython), repr(VCVER_KIND_STR[kind_t.kind]), repr(msvc_version)
    )
    return is_vcforpython

def msvc_version_skip_uwp_target(env, msvc_version):

    vernum = float(Util.get_msvc_version_prefix(msvc_version))
    vernum_int = int(vernum * 10)

    if vernum_int != 140:
        return False

    kind_t = _msvc_version_kind_lookup(msvc_version, env)
    if kind_t.kind != VCVER_KIND_EXPRESS:
        return False

    target_arch = env.get('TARGET_ARCH')

    uwp_is_supported = kind_t.extended.get('uwp_is_supported', {})
    is_supported = uwp_is_supported.get(target_arch, True)

    if is_supported:
        return False

    return True

def _pdir_detect_binaries(pdir, detect):

    vs_root = os.path.join(pdir, detect.root)
    ide_path = os.path.join(vs_root, detect.path)

    bitfields = 0b_0000
    for ide_program in detect.programs:
        prog = os.path.join(ide_path, ide_program.program)
        if not os.path.exists(prog):
            continue
        bitfields |= ide_program.bitfield

    have_dev = bool(bitfields & BITFIELD_KIND_DEVELOP)
    have_exp = bool(bitfields & BITFIELD_KIND_EXPRESS)
    have_exp_win = bool(bitfields & BITFIELD_KIND_EXPRESS_WIN)
    have_exp_web = bool(bitfields & BITFIELD_KIND_EXPRESS_WEB)

    binaries_t = VCVER_DETECT_BINARIES(
        bitfields=bitfields,
        have_dev=have_dev,
        have_exp=have_exp,
        have_exp_win=have_exp_win,
        have_exp_web=have_exp_web,
    )

    debug(
        'vs_root=%s, dev=%s, exp=%s, exp_win=%s, exp_web=%s, pdir=%s',
        repr(vs_root),
        binaries_t.have_dev, binaries_t.have_exp,
        binaries_t.have_exp_win, binaries_t.have_exp_web,
        repr(pdir)
    )

    return vs_root, binaries_t

_cache_pdir_vswhere_kind = {}

def msvc_version_pdir_vswhere_kind(msvc_version, pdir, detect_t):
    global _cache_pdir_vswhere_kind

    vc_dir = os.path.normcase(os.path.normpath(pdir))
    cache_key = (msvc_version, vc_dir)

    rval = _cache_pdir_vswhere_kind.get(cache_key)
    if rval is not None:
        debug('cache=%s', repr(rval))
        return rval

    extended = {}

    prefix, suffix = Util.get_msvc_version_prefix_suffix(msvc_version)

    vs_root, binaries_t = _pdir_detect_binaries(pdir, detect_t)

    if binaries_t.have_dev:
        kind = VCVER_KIND_DEVELOP
    elif binaries_t.have_exp:
        kind = VCVER_KIND_EXPRESS
    else:
        kind = VCVER_KIND_CMDLINE

    skip = False
    save = False

    if suffix != 'Exp' and kind == VCVER_KIND_EXPRESS:
        skip = True
        save = USE_EXPRESS_FOR_NONEXPRESS
    elif suffix == 'Exp' and kind != VCVER_KIND_EXPRESS:
        skip = True

    kind_t = VCVER_DETECT_KIND(
        skip=skip,
        save=save,
        kind=kind,
        binaries_t=binaries_t,
        extended=extended,
    )

    debug(
        'skip=%s, save=%s, kind=%s, msvc_version=%s, pdir=%s',
        kind_t.skip, kind_t.save, repr(VCVER_KIND_STR[kind_t.kind]),
        repr(msvc_version), repr(pdir)
    )

    _cache_pdir_vswhere_kind[cache_key] = kind_t

    return kind_t

# VS2015 buildtools batch file call detection
#    vs2015 buildtools do not support sdk_version or UWP arguments

_VS2015BT_PATH = r'..\Microsoft Visual C++ Build Tools\vcbuildtools.bat'

_VS2015BT_REGEX_STR = ''.join([
    r'^\s*if\s+exist\s+',
    re.escape(fr'"%~dp0..\{_VS2015BT_PATH}"'),
    r'\s+goto\s+setup_buildsku\s*$',
])

_VS2015BT_VCVARS_BUILDTOOLS = re.compile(_VS2015BT_REGEX_STR, re.IGNORECASE)
_VS2015BT_VCVARS_STOP = re.compile(r'^\s*[:]Setup_VS\s*$', re.IGNORECASE)

def _vs_buildtools_2015_vcvars(vcvars_file):
    have_buildtools_vcvars = False
    with open(vcvars_file) as fh:
        for line in fh:
            if _VS2015BT_VCVARS_BUILDTOOLS.match(line):
                have_buildtools_vcvars = True
                break
            if _VS2015BT_VCVARS_STOP.match(line):
                break
    return have_buildtools_vcvars

def _vs_buildtools_2015(vs_root, vc_dir):

    is_btdispatch = False

    do_once = True
    while do_once:
        do_once = False

        buildtools_file = os.path.join(vs_root, _VS2015BT_PATH)
        have_buildtools = os.path.exists(buildtools_file)
        debug('have_buildtools=%s', have_buildtools)
        if not have_buildtools:
            break

        vcvars_file = os.path.join(vc_dir, 'vcvarsall.bat')
        have_vcvars = os.path.exists(vcvars_file)
        debug('have_vcvars=%s', have_vcvars)
        if not have_vcvars:
            break

        have_buildtools_vcvars = _vs_buildtools_2015_vcvars(vcvars_file)
        debug('have_buildtools_vcvars=%s', have_buildtools_vcvars)
        if not have_buildtools_vcvars:
            break

        is_btdispatch = True

    debug('is_btdispatch=%s', is_btdispatch)
    return is_btdispatch

_VS2015EXP_VCVARS_LIBPATH = re.compile(
    ''.join([
        r'^\s*\@if\s+exist\s+\"\%VCINSTALLDIR\%LIB\\store\\(amd64|arm)"\s+',
        r'set (LIB|LIBPATH)=\%VCINSTALLDIR\%LIB\\store\\(amd64|arm);.*\%(LIB|LIBPATH)\%\s*$'
    ]),
    re.IGNORECASE
)

_VS2015EXP_VCVARS_STOP = re.compile(r'^\s*[:]GetVSCommonToolsDir\s*$', re.IGNORECASE)

def _vs_express_2015_vcvars(vcvars_file):
    n_libpath = 0
    with open(vcvars_file) as fh:
        for line in fh:
            if _VS2015EXP_VCVARS_LIBPATH.match(line):
                n_libpath += 1
            elif _VS2015EXP_VCVARS_STOP.match(line):
                break
    have_uwp_fix = n_libpath >= 2
    return have_uwp_fix

def _vs_express_2015(pdir):

    have_uwp_amd64 = False
    have_uwp_arm = False

    vcvars_file = os.path.join(pdir, r'vcvarsall.bat')
    if os.path.exists(vcvars_file):

        vcvars_file = os.path.join(pdir, r'bin\x86_amd64\vcvarsx86_amd64.bat')
        if os.path.exists(vcvars_file):
            have_uwp_fix = _vs_express_2015_vcvars(vcvars_file)
            if have_uwp_fix:
                have_uwp_amd64 = True

        vcvars_file = os.path.join(pdir, r'bin\x86_arm\vcvarsx86_arm.bat')
        if os.path.exists(vcvars_file):
            have_uwp_fix = _vs_express_2015_vcvars(vcvars_file)
            if have_uwp_fix:
                have_uwp_arm = True

    debug('have_uwp_amd64=%s, have_uwp_arm=%s', have_uwp_amd64, have_uwp_arm)
    return have_uwp_amd64, have_uwp_arm

# winsdk installed 2010 [7.1], 2008 [7.0, 6.1] folders

_REGISTRY_WINSDK_VERSIONS = {'10.0', '9.0'}

_cache_pdir_registry_winsdk = {}

def _msvc_version_pdir_registry_winsdk(msvc_version, pdir):
    global _cache_pdir_registry_winsdk

    # detect winsdk-only installations
    #
    #     registry keys:
    #         [prefix]\VisualStudio\SxS\VS7\10.0  <undefined>
    #         [prefix]\VisualStudio\SxS\VC7\10.0  product directory
    #         [prefix]\VisualStudio\SxS\VS7\9.0   <undefined>
    #         [prefix]\VisualStudio\SxS\VC7\9.0   product directory
    #
    #     winsdk notes:
    #         - winsdk installs do not define the common tools env var
    #         - the product dir is detected but the vcvars batch files will fail
    #         - regular installations populate the VS7 registry keys
    #

    vc_dir = os.path.normcase(os.path.normpath(pdir))
    cache_key = (msvc_version, vc_dir)

    rval = _cache_pdir_registry_winsdk.get(cache_key)
    if rval is not None:
        debug('cache=%s', repr(rval))
        return rval

    if msvc_version not in _REGISTRY_WINSDK_VERSIONS:

        is_sdk = False

        debug('is_sdk=%s, msvc_version=%s', is_sdk, repr(msvc_version))

    else:

        vc_dir = os.path.normcase(os.path.normpath(pdir))

        vc_suffix = Registry.vstudio_sxs_vc7(msvc_version)
        vc_qresults = [record[0] for record in Registry.microsoft_query_paths(vc_suffix)]
        vc_root = os.path.normcase(os.path.normpath(vc_qresults[0])) if vc_qresults else None

        if vc_dir != vc_root:
            # registry vc path is not the current pdir

            is_sdk = False

            debug(
                'is_sdk=%s, msvc_version=%s, pdir=%s, vc_root=%s',
                is_sdk, repr(msvc_version), repr(vc_dir), repr(vc_root)
            )

        else:
            # registry vc path is the current pdir

            vs_suffix = Registry.vstudio_sxs_vs7(msvc_version)
            vs_qresults = [record[0] for record in Registry.microsoft_query_paths(vs_suffix)]
            vs_root = vs_qresults[0] if vs_qresults else None

            is_sdk = bool(not vs_root and vc_root)

            debug(
                'is_sdk=%s, msvc_version=%s, vs_root=%s, vc_root=%s',
                is_sdk, repr(msvc_version), repr(vs_root), repr(vc_root)
            )

    _cache_pdir_registry_winsdk[cache_key] = is_sdk

    return is_sdk

_cache_pdir_registry_kind = {}

def msvc_version_pdir_registry_kind(msvc_version, pdir, detect_t, is_vcforpython=False):
    global _cache_pdir_registry_kind

    vc_dir = os.path.normcase(os.path.normpath(pdir))
    cache_key = (msvc_version, vc_dir)

    rval = _cache_pdir_registry_kind.get(cache_key)
    if rval is not None:
        debug('cache=%s', repr(rval))
        return rval

    extended = {}

    prefix, suffix = Util.get_msvc_version_prefix_suffix(msvc_version)

    vs_root, binaries_t = _pdir_detect_binaries(pdir, detect_t)

    if binaries_t.have_dev:
        kind = VCVER_KIND_DEVELOP
    elif binaries_t.have_exp:
        kind = VCVER_KIND_EXPRESS
    elif msvc_version == '14.0' and _vs_buildtools_2015(vs_root, pdir):
        kind = VCVER_KIND_BTDISPATCH
    elif msvc_version == '9.0' and is_vcforpython:
        kind = VCVER_KIND_VCFORPYTHON
    elif binaries_t.have_exp_win:
        kind = VCVER_KIND_EXPRESS_WIN
    elif binaries_t.have_exp_web:
        kind = VCVER_KIND_EXPRESS_WEB
    elif _msvc_version_pdir_registry_winsdk(msvc_version, pdir):
        kind = VCVER_KIND_SDK
    else:
        kind = VCVER_KIND_CMDLINE

    skip = False
    save = False

    if kind in (VCVER_KIND_EXPRESS_WIN, VCVER_KIND_EXPRESS_WEB, VCVER_KIND_SDK):
        skip = True
    elif suffix != 'Exp' and kind == VCVER_KIND_EXPRESS:
        skip = True
        save = USE_EXPRESS_FOR_NONEXPRESS
    elif suffix == 'Exp' and kind != VCVER_KIND_EXPRESS:
        skip = True

    if prefix == '14.0' and kind == VCVER_KIND_EXPRESS:
        have_uwp_amd64, have_uwp_arm = _vs_express_2015(pdir)
        uwp_is_supported = {
            'x86': True,
            'amd64': have_uwp_amd64,
            'arm': have_uwp_arm,
        }
        extended['uwp_is_supported'] = uwp_is_supported

    kind_t = VCVER_DETECT_KIND(
        skip=skip,
        save=save,
        kind=kind,
        binaries_t=binaries_t,
        extended=extended,
    )

    debug(
        'skip=%s, save=%s, kind=%s, msvc_version=%s, pdir=%s',
        kind_t.skip, kind_t.save, repr(VCVER_KIND_STR[kind_t.kind]),
        repr(msvc_version), repr(pdir)
    )

    _cache_pdir_registry_kind[cache_key] = kind_t

    return kind_t

# queries

def get_msvc_version_kind(msvc_version, env=None):
    kind_t = _msvc_version_kind_lookup(msvc_version, env)
    kind_str = VCVER_KIND_STR[kind_t.kind]
    debug(
        'kind=%s, kind_str=%s, msvc_version=%s',
        repr(kind_t.kind), repr(kind_str), repr(msvc_version)
    )
    return (kind_t.kind, kind_str)

def msvc_version_sdk_version_is_supported(msvc_version, env=None):

    vernum = float(Util.get_msvc_version_prefix(msvc_version))
    vernum_int = int(vernum * 10)

    kind_t = _msvc_version_kind_lookup(msvc_version, env)

    if vernum_int >= 141:
        # VS2017 and later
        is_supported = True
    elif vernum_int == 140:
        # VS2015:
        #   True:  Develop, CmdLine
        #   False: Express, BTDispatch
        is_supported = True
        if kind_t.kind == VCVER_KIND_EXPRESS:
            is_supported = False
        elif kind_t.kind == VCVER_KIND_BTDISPATCH:
            is_supported = False
    else:
        # VS2013 and earlier
        is_supported = False

    debug(
        'is_supported=%s, msvc_version=%s, kind=%s',
        is_supported, repr(msvc_version), repr(VCVER_KIND_STR[kind_t.kind])
    )
    return is_supported

def msvc_version_uwp_is_supported(msvc_version, target_arch=None, env=None):

    vernum = float(Util.get_msvc_version_prefix(msvc_version))
    vernum_int = int(vernum * 10)

    kind_t = _msvc_version_kind_lookup(msvc_version, env)

    is_target = False

    if vernum_int >= 141:
        # VS2017 and later
        is_supported = True
    elif vernum_int == 140:
        # VS2015:
        #   True:  Develop, CmdLine
        #   Maybe: Express
        #   False: BTDispatch
        is_supported = True
        if kind_t.kind == VCVER_KIND_EXPRESS:
            uwp_is_supported = kind_t.extended.get('uwp_is_supported', {})
            is_supported = uwp_is_supported.get(target_arch, True)
            is_target = True
        elif kind_t.kind == VCVER_KIND_BTDISPATCH:
            is_supported = False
    else:
        # VS2013 and earlier
        is_supported = False

    debug(
        'is_supported=%s, is_target=%s, msvc_version=%s, kind=%s, target_arch=%s',
        is_supported, is_target, repr(msvc_version), repr(VCVER_KIND_STR[kind_t.kind]), repr(target_arch)
    )

    return is_supported, is_target

# reset cache

def reset() -> None:
    global _cache_vcver_kind_map
    global _cache_pdir_vswhere_kind
    global _cache_pdir_registry_kind
    global _cache_pdir_registry_winsdk

    debug('')

    _cache_vcver_kind_map = {}
    _cache_pdir_vswhere_kind = {}
    _cache_pdir_registry_kind = {}
    _cache_pdir_registry_winsdk = {}
