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
Constants and initialized data structures for Microsoft Visual C/C++.
"""

from collections import (
    namedtuple,
)

from .Exceptions import (
    MSVCInternalError,
)

from . import Dispatcher
Dispatcher.register_modulename(__name__)


UNDEFINED = object()

BOOLEAN_SYMBOLS = {}
BOOLEAN_EXTERNAL = {}

for bool_val, symbol_list, symbol_case_list in [
    (False, (False, 0, '0', None, ''), ('False', 'No',  'F', 'N')),
    (True,  (True,  1, '1'),           ('True',  'Yes', 'T', 'Y')),
]:
    BOOLEAN_SYMBOLS[bool_val] = list(symbol_list)
    for symbol in symbol_case_list:
        BOOLEAN_SYMBOLS[bool_val].extend([symbol, symbol.lower(), symbol.upper()])

    for symbol in BOOLEAN_SYMBOLS[bool_val]:
        BOOLEAN_EXTERNAL[symbol] = bool_val

MSVC_PLATFORM_DEFINITION = namedtuple('MSVCPlatform', [
    'vc_platform',
    'is_uwp',
])

MSVC_PLATFORM_DEFINITION_LIST = []

MSVC_PLATFORM_INTERNAL = {}
MSVC_PLATFORM_EXTERNAL = {}

for vc_platform, is_uwp in [
    ('Desktop', False),
    ('UWP', True),
]:

    vc_platform_def = MSVC_PLATFORM_DEFINITION(
        vc_platform = vc_platform,
        is_uwp = is_uwp,
    )

    MSVC_PLATFORM_DEFINITION_LIST.append(vc_platform_def)

    MSVC_PLATFORM_INTERNAL[vc_platform] = vc_platform_def

    for symbol in [vc_platform, vc_platform.lower(), vc_platform.upper()]:
        MSVC_PLATFORM_EXTERNAL[symbol] = vc_platform_def

MSVC_RUNTIME_DEFINITION = namedtuple('MSVCRuntime', [
    'vc_runtime',
    'vc_runtime_numeric',
    'vc_runtime_alias_list',
    'vc_runtime_vsdef_list',
])

MSVC_RUNTIME_DEFINITION_LIST = []

MSVC_RUNTIME_INTERNAL = {}
MSVC_RUNTIME_EXTERNAL = {}

for vc_runtime, vc_runtime_numeric, vc_runtime_alias_list in [
    ('140', 140, ['ucrt']),
    ('120', 120, ['msvcr120']),
    ('110', 110, ['msvcr110']),
    ('100', 100, ['msvcr100']),
    ('90',   90, ['msvcr90']),
    ('80',   80, ['msvcr80']),
    ('71',   71, ['msvcr71']),
    ('70',   70, ['msvcr70']),
    ('60',   60, ['msvcrt']),
]:
    vc_runtime_def = MSVC_RUNTIME_DEFINITION(
        vc_runtime = vc_runtime,
        vc_runtime_numeric = vc_runtime_numeric,
        vc_runtime_alias_list = vc_runtime_alias_list,
        vc_runtime_vsdef_list = [],
    )

    MSVC_RUNTIME_DEFINITION_LIST.append(vc_runtime_def)

    MSVC_RUNTIME_INTERNAL[vc_runtime] = vc_runtime_def
    MSVC_RUNTIME_EXTERNAL[vc_runtime] = vc_runtime_def

    for vc_runtime_alias in vc_runtime_alias_list:
        MSVC_RUNTIME_EXTERNAL[vc_runtime_alias] = vc_runtime_def

MSVC_BUILDSERIES_DEFINITION = namedtuple('MSVCBuildSeries', [
    'vc_buildseries',
    'vc_buildseries_numeric',
    'vc_version',
    'vc_version_numeric',
    'cl_version',
    'cl_version_numeric',
])

MSVC_BUILDSERIES_DEFINITION_LIST = []

MSVC_BUILDSERIES_INTERNAL = {}
MSVC_BUILDSERIES_EXTERNAL = {}

VC_BUILDTOOLS_MAP = {}

VC_VERSION_MAP = {}
CL_VERSION_MAP = {}

for (vc_buildseries, vc_version, cl_version) in [
    ('144', '14.4', '19.4'),
    ('143', '14.3', '19.3'),
    ('142', '14.2', '19.2'),
    ('141', '14.1', '19.1'),
    ('140', '14.0', '19.0'),
    ('120', '12.0', '18.0'),
    ('110', '11.0', '17.0'),
    ('100', '10.0', '16.0'),
    ('90', '9.0', '15.0'),
    ('80', '8.0', '14.0'),
    ('71', '7.1', '13.1'),
    ('70', '7.0', '13.0'),
    ('60', '6.0', '12.0'),
]:

    vc_buildseries_def = MSVC_BUILDSERIES_DEFINITION(
        vc_buildseries=vc_buildseries,
        vc_buildseries_numeric=int(vc_buildseries),
        vc_version=vc_version,
        vc_version_numeric=float(vc_version),
        cl_version=cl_version,
        cl_version_numeric=float(cl_version),
    )

    MSVC_BUILDSERIES_DEFINITION_LIST.append(vc_buildseries_def)

    MSVC_BUILDSERIES_INTERNAL[vc_buildseries] = vc_buildseries_def
    MSVC_BUILDSERIES_EXTERNAL[vc_buildseries] = vc_buildseries_def
    MSVC_BUILDSERIES_EXTERNAL[vc_version] = vc_buildseries_def

    VC_VERSION_MAP[vc_version] = vc_buildseries_def
    CL_VERSION_MAP[cl_version] = vc_buildseries_def

MSVC_BUILDTOOLS_DEFINITION = namedtuple('MSVCBuildtools', [
    'vc_buildtools',
    'vc_buildtools_numeric',
    'vc_buildseries_list',
    'vc_runtime_def',
    'vc_istoolset',
    'msvc_version',
    'msvc_version_numeric',
])

MSVC_BUILDTOOLS_DEFINITION_LIST = []

MSVC_BUILDTOOLS_INTERNAL = {}
MSVC_BUILDTOOLS_EXTERNAL = {}

MSVC_VERSION_NEWEST = None
MSVC_VERSION_NEWEST_NUMERIC = 0.0

for vc_buildtools, vc_buildseries_list, vc_runtime, vc_istoolset in [
    ('v143', ['144', '143'], '140', True),
    ('v142', ['142'], '140', True),
    ('v141', ['141'], '140', True),
    ('v140', ['140'], '140', True),
    ('v120', ['120'], '120', False),
    ('v110', ['110'], '110', False),
    ('v100', ['100'], '100', False),
    ('v90', ['90'], '90', False),
    ('v80', ['80'], '80', False),
    ('v71', ['71'], '71', False),
    ('v70', ['70'], '70', False),
    ('v60', ['60'], '60', False),
]:

    vc_runtime_def = MSVC_RUNTIME_INTERNAL[vc_runtime]

    vc_buildseries_list = tuple(
        MSVC_BUILDSERIES_INTERNAL[vc_buildseries]
        for vc_buildseries in vc_buildseries_list
    )

    vc_buildtools_numstr = vc_buildtools[1:]

    msvc_version = vc_buildtools_numstr[:-1] + '.' + vc_buildtools_numstr[-1]
    msvc_version_numeric = float(msvc_version)

    vc_buildtools_def = MSVC_BUILDTOOLS_DEFINITION(
        vc_buildtools = vc_buildtools,
        vc_buildtools_numeric = int(vc_buildtools[1:]),
        vc_buildseries_list = vc_buildseries_list,
        vc_runtime_def = vc_runtime_def,
        vc_istoolset = vc_istoolset,
        msvc_version = msvc_version,
        msvc_version_numeric = msvc_version_numeric,
    )

    MSVC_BUILDTOOLS_DEFINITION_LIST.append(vc_buildtools_def)

    MSVC_BUILDTOOLS_INTERNAL[vc_buildtools] = vc_buildtools_def
    MSVC_BUILDTOOLS_EXTERNAL[vc_buildtools] = vc_buildtools_def
    MSVC_BUILDTOOLS_EXTERNAL[msvc_version] = vc_buildtools_def

    for vc_buildseries_def in vc_buildseries_list:
        VC_BUILDTOOLS_MAP[vc_buildseries_def.vc_buildseries] = vc_buildtools_def

    if vc_buildtools_def.msvc_version_numeric > MSVC_VERSION_NEWEST_NUMERIC:
        MSVC_VERSION_NEWEST_NUMERIC = vc_buildtools_def.msvc_version_numeric
        MSVC_VERSION_NEWEST = vc_buildtools_def.msvc_version

MSVS_VERSION_INTERNAL = {}
MSVS_VERSION_EXTERNAL = {}

MSVC_VERSION_INTERNAL = {}
MSVC_VERSION_EXTERNAL = {}
MSVC_VERSION_SUFFIX = {}

MSVS_VERSION_MAJOR_MAP = {}

MSVC_SDK_VERSIONS = set()

VISUALSTUDIO_DEFINITION = namedtuple('VisualStudioDefinition', [
    'vs_product',
    'vs_product_alias_list',
    'vs_version',
    'vs_version_major',
    'vs_envvar',
    'vs_express',
    'vs_lookup',
    'vc_sdk_versions',
    'vc_ucrt_versions',
    'vc_uwp',
    'vc_buildtools_def',
    'vc_buildtools_all',
])

VISUALSTUDIO_DEFINITION_LIST = []

VS_PRODUCT_ALIAS = {
    '1998': ['6']
}

# vs_envvar: VisualStudioVersion defined in environment for MSVS 2012 and later
#            MSVS 2010 and earlier cl_version -> vs_def is a 1:1 mapping
# SDK attached to product or buildtools?
for vs_product, vs_version, vs_envvar, vs_express, vs_lookup, vc_sdk, vc_ucrt, vc_uwp, vc_buildtools_all in [
    ('2022', '17.0', True,  False, 'vswhere' , ['10.0', '8.1'], ['10'],   'uwp', ['v143', 'v142', 'v141', 'v140']),
    ('2019', '16.0', True,  False, 'vswhere' , ['10.0', '8.1'], ['10'],   'uwp', ['v142', 'v141', 'v140']),
    ('2017', '15.0', True,  True,  'vswhere' , ['10.0', '8.1'], ['10'],   'uwp', ['v141', 'v140']),
    ('2015', '14.0', True,  True,  'registry', ['10.0', '8.1'], ['10'], 'store', ['v140']),
    ('2013', '12.0', True,  True,  'registry',            None,   None,    None, ['v120']),
    ('2012', '11.0', True,  True,  'registry',            None,   None,    None, ['v110']),
    ('2010', '10.0', False, True,  'registry',            None,   None,    None, ['v100']),
    ('2008',  '9.0', False, True,  'registry',            None,   None,    None, ['v90']),
    ('2005',  '8.0', False, True,  'registry',            None,   None,    None, ['v80']),
    ('2003',  '7.1', False, False, 'registry',            None,   None,    None, ['v71']),
    ('2002',  '7.0', False, False, 'registry',            None,   None,    None, ['v70']),
    ('1998',  '6.0', False, False, 'registry',            None,   None,    None, ['v60']),
]:

    vs_version_major = vs_version.split('.')[0]

    vc_buildtools_def = MSVC_BUILDTOOLS_INTERNAL[vc_buildtools_all[0]]

    vs_def = VISUALSTUDIO_DEFINITION(
        vs_product = vs_product,
        vs_product_alias_list = [],
        vs_version = vs_version,
        vs_version_major = vs_version_major,
        vs_envvar = vs_envvar,
        vs_express = vs_express,
        vs_lookup = vs_lookup,
        vc_sdk_versions = vc_sdk,
        vc_ucrt_versions = vc_ucrt,
        vc_uwp = vc_uwp,
        vc_buildtools_def = vc_buildtools_def,
        vc_buildtools_all = vc_buildtools_all,
    )

    VISUALSTUDIO_DEFINITION_LIST.append(vs_def)

    vc_buildtools_def.vc_runtime_def.vc_runtime_vsdef_list.append(vs_def)

    msvc_version = vc_buildtools_def.msvc_version

    MSVS_VERSION_INTERNAL[vs_product] = vs_def
    MSVS_VERSION_EXTERNAL[vs_product] = vs_def
    MSVS_VERSION_EXTERNAL[vs_version] = vs_def

    MSVC_VERSION_INTERNAL[msvc_version] = vs_def
    MSVC_VERSION_EXTERNAL[vs_product] = vs_def
    MSVC_VERSION_EXTERNAL[msvc_version] = vs_def
    MSVC_VERSION_EXTERNAL[vc_buildtools_def.vc_buildtools] = vs_def

    if vs_product in VS_PRODUCT_ALIAS:
        for vs_product_alias in VS_PRODUCT_ALIAS[vs_product]:
            vs_def.vs_product_alias_list.append(vs_product_alias)
            MSVS_VERSION_EXTERNAL[vs_product_alias] = vs_def
            MSVC_VERSION_EXTERNAL[vs_product_alias] = vs_def

    MSVC_VERSION_SUFFIX[msvc_version] = vs_def
    if vs_express:
        MSVC_VERSION_SUFFIX[msvc_version + 'Exp'] = vs_def

    MSVS_VERSION_MAJOR_MAP[vs_version_major] = vs_def

    if vc_sdk:
        MSVC_SDK_VERSIONS.update(vc_sdk)

# EXPERIMENTAL: msvc version/toolset search lists
#
# VS2017 example:
#
#     defaults['14.1']    = ['14.1', '14.1Exp']
#     defaults['14.1Exp'] = ['14.1Exp']
#
#     search['14.1']    = ['14.3', '14.2', '14.1', '14.1Exp']
#     search['14.1Exp'] = ['14.1Exp']

MSVC_VERSION_TOOLSET_DEFAULTS_MAP = {}
MSVC_VERSION_TOOLSET_SEARCH_MAP = {}

# Pass 1: Build defaults lists and setup express versions
for vs_def in VISUALSTUDIO_DEFINITION_LIST:
    if not vs_def.vc_buildtools_def.vc_istoolset:
        continue
    version_key = vs_def.vc_buildtools_def.msvc_version
    MSVC_VERSION_TOOLSET_DEFAULTS_MAP[version_key] = [version_key]
    MSVC_VERSION_TOOLSET_SEARCH_MAP[version_key] = []
    if vs_def.vs_express:
        express_key = version_key + 'Exp'
        MSVC_VERSION_TOOLSET_DEFAULTS_MAP[version_key].append(express_key)
        MSVC_VERSION_TOOLSET_DEFAULTS_MAP[express_key] = [express_key]
        MSVC_VERSION_TOOLSET_SEARCH_MAP[express_key] = [express_key]

# Pass 2: Extend search lists (decreasing version order)
for vs_def in VISUALSTUDIO_DEFINITION_LIST:
    if not vs_def.vc_buildtools_def.vc_istoolset:
        continue
    version_key = vs_def.vc_buildtools_def.msvc_version
    for vc_buildtools in vs_def.vc_buildtools_all:
        toolset_buildtools_def = MSVC_BUILDTOOLS_INTERNAL[vc_buildtools]
        toolset_vs_def = MSVC_VERSION_INTERNAL[toolset_buildtools_def.msvc_version]
        buildtools_key = toolset_buildtools_def.msvc_version
        MSVC_VERSION_TOOLSET_SEARCH_MAP[buildtools_key].extend(MSVC_VERSION_TOOLSET_DEFAULTS_MAP[version_key])

# convert string version set to string version list ranked in descending order
MSVC_SDK_VERSIONS = [str(f) for f in sorted([float(s) for s in MSVC_SDK_VERSIONS], reverse=True)]


def verify():
    from . import Util
    from .. import vc
    for msvc_version in vc._VCVER:
        if msvc_version not in MSVC_VERSION_SUFFIX:
            err_msg = f'msvc_version {msvc_version!r} not in MSVC_VERSION_SUFFIX'
            raise MSVCInternalError(err_msg)
        vc_version = Util.get_msvc_version_prefix(msvc_version)
        if vc_version not in MSVC_VERSION_INTERNAL:
            err_msg = f'vc_version {vc_version!r} not in MSVC_VERSION_INTERNAL'
            raise MSVCInternalError(err_msg)

