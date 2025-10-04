#!/bin/python

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
