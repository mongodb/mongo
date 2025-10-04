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
# init.py
#      This is installed as __init__.py, and imports the file created by SWIG.
# This is needed because SWIG's import helper code created by certain SWIG
# versions may be broken, see: https://github.com/swig/swig/issues/769 .
# Importing indirectly seems to avoid these issues.

import os, sys
fname = os.path.basename(__file__)
if fname != '__init__.py' and fname != '__init__.pyc':
    print(__file__ + ': this file is not yet installed')
    sys.exit(1)

if sys.version_info[0] <= 2:
    print('WiredTiger requires Python version 3.0 or above')
    sys.exit(1)

# Restart this instance of python so it uses the most recent os.environ values
def restart_python():
    python = sys.executable
    os.execl(python, os.path.abspath(__file__), *sys.argv)

# After importing the SWIG-generated file, copy all symbols from it
# to this module so they will appear in the wiredtiger namespace.
me = sys.modules[__name__]
sys.path.append(os.path.dirname(__file__))

# Find the sanitizer environment variable.
# FIXME-WT-13237: Rename these testutil flags
if os.environ.get("TESTUTIL_TSAN") == "1":
    import subprocess

    # FIXME-WT-13143 We assume TSan is only compatible with clang here. This may change in the future.
    command = "clang --print-file-name libtsan.so.2"
    find_tsan_so = subprocess.run(command, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    tsan_so_path = find_tsan_so.stdout.strip()
    if not os.path.isfile(tsan_so_path):
        print("Cannot find tsan lib")
        exit(1)

    ld_preload = os.environ.get("LD_PRELOAD")
    if not ld_preload:
        os.environ["LD_PRELOAD"] = tsan_so_path
        restart_python()
    elif tsan_so_path not in os.environ["LD_PRELOAD"]:
        os.environ["LD_PRELOAD"] += f":{tsan_so_path}"
        restart_python()
    else:
        # Python already has TSan linking, but if python calls ./wt in a subprocess this breaks ./wt
        # Remove the TSan libs from LD_PRELOAD but *don't* restart python so we still have TSan linking in the current running process. 
        paths = os.environ["LD_PRELOAD"].split(":")
        os.environ["LD_PRELOAD"] = ":".join([p for p in paths if p != tsan_so_path])

# explicitly importing _wiredtiger in advance of SWIG allows us to not
# use relative importing, as SWIG does.  It doesn't work for us with Python2.
import _wiredtiger
import swig_wiredtiger
for name in dir(swig_wiredtiger):
    value = getattr(swig_wiredtiger, name)
    setattr(me, name, value)
