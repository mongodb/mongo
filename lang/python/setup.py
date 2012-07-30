#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
#   All rights reserved.
#
# See the file LICENSE for redistribution information.

import re, os, sys
from distutils.core import setup, Extension

# OS X hack: turn off the Universal binary support that is built into the
# Python build machinery, just build for the default CPU architecture.
if not 'ARCHFLAGS' in os.environ:
    os.environ['ARCHFLAGS'] = ''

# Suppress warnings building SWIG code on OS X 10.8
extra_cflags = []
if sys.platform == 'darwin':
    kernel_version = os.uname()[2] # e.g. 12.0.0 is Mountain Lion
    major_version = int(kernel_version.split('.')[0])
    if major_version >= 12:
        extra_cflags += ['-Wno-self-assign', '-Qunused-arguments']

dir = os.path.dirname(__file__)

# Read the version information from dist/RELEASE
dist = os.path.join(os.path.dirname(os.path.dirname(dir)), 'dist')
for l in open(os.path.join(dist, 'RELEASE')):
    if re.match(r'WIREDTIGER_VERSION_(?:MAJOR|MINOR|PATCH)=', l):
        exec(l)

wt_ver = '%d.%d' % (WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR)

setup(name='wiredtiger', version=wt_ver,
    ext_modules=[Extension('_wiredtiger',
        [os.path.join(dir, 'wiredtiger_wrap.c')],
        include_dirs=['../..'],
        library_dirs=['../../.libs'],
        libraries=['wiredtiger'],
        extra_compile_args=extra_cflags,
    )],
    py_modules=['wiredtiger'],
)
