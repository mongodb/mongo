#!/usr/bin/env python3
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

# Common functions used by python scripts in this directory.

import os, sys

# Set the system path to include the python build directory if we can find it.
def setup_python_path():
    # Assuming we're somewhere in a build directory, walk the tree up
    # looking for the wt program.
    curdir = os.getcwd()
    d = curdir
    found = False
    while d != '/':
        if os.path.isfile(os.path.join(d, 'wt')) or os.path.isfile(os.path.join(d, 'wt.exe')):
            found = True
            break
        d = os.path.dirname(d)
    if found:
        sys.path.insert(1, os.path.join(d, 'lang', 'python'))
    else:
        print('Cannot find wt, must run this from a build directory')
        sys.exit(1)

# Import the wiredtiger directory and return the wiredtiger_open function.
def import_wiredtiger():
    try:
        from wiredtiger import wiredtiger_open
    except:
        setup_python_path()
        from wiredtiger import wiredtiger_open
    return wiredtiger_open

# This name will be exported
wiredtiger_open = import_wiredtiger()
