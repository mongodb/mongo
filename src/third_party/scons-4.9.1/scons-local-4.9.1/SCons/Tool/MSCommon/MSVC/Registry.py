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
Windows registry functions for Microsoft Visual C/C++.
"""

import os

from SCons.Util import (
    HKEY_LOCAL_MACHINE,
    HKEY_CURRENT_USER,
    RegGetValue,
)

from .. common import (
    debug,
)

from . import Util

from . import Dispatcher
Dispatcher.register_modulename(__name__)


# A null-terminated string that contains unexpanded references to environment variables.
REG_EXPAND_SZ = 2

def read_value(hkey, subkey_valname, expand: bool=True):
    try:
        rval_t = RegGetValue(hkey, subkey_valname)
    except OSError:
        debug('OSError: hkey=%s, subkey=%s', repr(hkey), repr(subkey_valname))
        return None
    rval, regtype = rval_t
    if regtype == REG_EXPAND_SZ and expand:
        rval = os.path.expandvars(rval)
    debug('hkey=%s, subkey=%s, rval=%s', repr(hkey), repr(subkey_valname), repr(rval))
    return rval

def registry_query_path(key, val, suffix, expand: bool=True):
    extval = val + '\\' + suffix if suffix else val
    qpath = read_value(key, extval, expand=expand)
    if qpath and os.path.exists(qpath):
        qpath = Util.normalize_path(qpath)
    else:
        qpath = None
    return (qpath, key, val, extval)

REG_SOFTWARE_MICROSOFT = [
    (HKEY_LOCAL_MACHINE, r'Software\Wow6432Node\Microsoft'),
    (HKEY_CURRENT_USER,  r'Software\Wow6432Node\Microsoft'), # SDK queries
    (HKEY_LOCAL_MACHINE, r'Software\Microsoft'),
    (HKEY_CURRENT_USER,  r'Software\Microsoft'),
]

def microsoft_query_paths(suffix, usrval=None, expand: bool=True):
    paths = []
    records = []
    for key, val in REG_SOFTWARE_MICROSOFT:
        extval = val + '\\' + suffix if suffix else val
        qpath = read_value(key, extval, expand=expand)
        if qpath and os.path.exists(qpath):
            qpath = Util.normalize_path(qpath)
            if qpath not in paths:
                paths.append(qpath)
                records.append((qpath, key, val, extval, usrval))
    return records

def microsoft_query_keys(suffix, usrval=None, expand: bool=True):
    records = []
    for key, val in REG_SOFTWARE_MICROSOFT:
        extval = val + '\\' + suffix if suffix else val
        rval = read_value(key, extval, expand=expand)
        if rval:
            records.append((rval, key, val, extval, usrval))
    return records

def microsoft_sdks(version):
    return '\\'.join([r'Microsoft SDKs\Windows', 'v' + version, r'InstallationFolder'])

def sdk_query_paths(version):
    q = microsoft_sdks(version)
    return microsoft_query_paths(q)

def windows_kits(version):
    return r'Windows Kits\Installed Roots\KitsRoot' + version

def windows_kit_query_paths(version):
    q = windows_kits(version)
    return microsoft_query_paths(q)

def vstudio_sxs_vs7(version):
    return '\\'.join([r'VisualStudio\SxS\VS7', version])

def vstudio_sxs_vc7(version):
    return '\\'.join([r'VisualStudio\SxS\VC7', version])

def devdiv_vs_servicing_component(version, component):
    return '\\'.join([r'DevDiv\VS\Servicing', version, component, 'Install'])

