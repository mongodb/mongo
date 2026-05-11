#!/bin/python
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

# Load our custom debugging gdb scripts into gdb.
# This can be called manually via `source /path/to/load_gdb_scripts.py`.
# gdb can also auto-run this script if it is located in the same folder 
# as the .so file and has an identical name with -gdb.py appended. For example: 
#     build/
#         libwiredtiger.so.11.2.0
#         libwiredtiger.so.11.2.0-gdb.py // this script
# 
# This file is copied into the build directory and renamed accordingly when we compile 
# WiredTiger with -DENABLE_SHARED=1.
# 
# For more information see https://sourceware.org/gdb/onlinedocs/gdb/objfile_002dgdbdotext-file.html

import gdb, sys, os

print("Loading custom WiredTiger gdb scripts...")

# Add the build directory to our path so we can import from the gdb_scripts subfolder.
build_dir = os.path.dirname(__file__)
sys.path.append(build_dir)

# Load gdb scripts written in python.
import gdb_scripts.hazard_pointers
import gdb_scripts.dump_insert_list

# load gdb scripts written in scheme.
gdb.execute(f"source {build_dir}/gdb_scripts/dump_row_int.gdb")
