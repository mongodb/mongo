#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
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
# runner/__init__.py
#   Used as a first import by runners, does any common initialization.
from __future__ import print_function

import os, sys
thisdir = os.path.dirname(os.path.abspath(__file__))
workgen_src = os.path.dirname(os.path.dirname(thisdir))
wt_dir = os.path.dirname(os.path.dirname(workgen_src))
curdir = os.getcwd()
env_builddir = os.getenv('WT_BUILDDIR')
if env_builddir:
    wt_builddir = env_builddir
elif os.path.isfile(os.path.join(curdir, 'wt')):
    wt_builddir = curdir
else:
    # Print a warning that we can't find a useable WiredTiger build. We will
    # proceed however in the chance the Python paths are set up correctly.
    print('Warning: Unable to identify WiredTiger build. Please consider either:\n'
            '- Setting \'WT_BUILDDIR\' environment variable to specify the build directory.\n'
            '- Calling workgen from the root of the build directory.')
    wt_builddir = ''

def _prepend_env_path(pathvar, s):
    last = ''
    try:
        last = ':' + os.environ[pathvar]
    except:
        pass
    os.environ[pathvar] = s + last

# Initialize the python path so needed modules can be imported.
# If the path already works, don't change it.
try:
    import wiredtiger
except:
    # We'll try hard to make the importing work, we'd like to runners
    # to be executable directly without having to set environment variables.
    sys.path.insert(0, os.path.join(wt_dir, 'lang', 'python'))
    sys.path.insert(0, os.path.join(wt_builddir, 'lang', 'python'))
    try:
        import wiredtiger
    except:
        # If the WiredTiger libraries is not in our library search path,
        # we need to set it and retry.  However, the dynamic link
        # library has already cached its value, our only option is
        # to restart the Python interpreter.
        if '_workgen_init' not in os.environ:
            os.environ['_workgen_init'] = 'true'
            libsdir = os.path.join(wt_builddir)
            _prepend_env_path('LD_LIBRARY_PATH', libsdir)
            _prepend_env_path('DYLD_LIBRARY_PATH', libsdir)
            py_args = sys.argv
            py_args.insert(0, sys.executable)
            try:
                os.execv(sys.executable, py_args)
            except Exception as exception:
                print('re-exec failed: ' + str(exception), file=sys.stderr)
                print('  exec(' + sys.executable + ', ' + str(py_args) + ')')
                print('Try adding "' + libsdir + '" to the', file=sys.stderr)
                print('LD_LIBRARY_PATH environment variable before running ' + \
                    'this program again.', file=sys.stderr)
                sys.exit(1)

try:
    import workgen
except:
    sys.path.insert(0, os.path.join(workgen_src, 'workgen'))
    sys.path.insert(0, os.path.join(wt_builddir, 'bench', 'workgen'))
    import workgen

from .core import txn, extensions_config, op_append, op_group_transaction, op_log_like, op_multi_table, op_populate_with_range, sleep, timed
from .latency import workload_latency
