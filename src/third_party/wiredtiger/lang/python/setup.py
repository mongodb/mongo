#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

import re, os, sys
from distutils.core import setup, Extension

# OS X hack: turn off the Universal binary support that is built into the
# Python build machinery, just build for the default CPU architecture.
if not 'ARCHFLAGS' in os.environ:
    os.environ['ARCHFLAGS'] = ''

# Suppress warnings building SWIG generated code
extra_cflags = [ '-w', '-I../../src/include']

dir = os.path.dirname(__file__)

# Read the version information from the RELEASE_INFO file
for l in open(os.path.join(dir, '..', '..', 'RELEASE_INFO')):
    if re.match(r'WIREDTIGER_VERSION_(?:MAJOR|MINOR|PATCH)=', l):
        exec(l)

wt_ver = '%d.%d' % (WIREDTIGER_VERSION_MAJOR, WIREDTIGER_VERSION_MINOR)

setup(name='wiredtiger', version=wt_ver,
    ext_modules=[Extension('_wiredtiger',
                [os.path.join(dir, 'wiredtiger_wrap.c')],
        libraries=['wiredtiger'],
        extra_compile_args=extra_cflags,
    )],
    package_dir={'' : dir},
    packages=['wiredtiger'],
)
