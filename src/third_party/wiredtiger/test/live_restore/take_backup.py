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
# take_backup.py
#   Take a backup of the database created by btree-500m-populate.wtperf to be used
#   as the source directory in live restore perf tests.
#   This script needs to be run from build/bench/wtperf and the btree-500m-populate
#   task must have run already to generate the WT_TEST_0_0 folder.

import os
import shutil
import sys

sys.path.append(os.path.abspath('../../lang/python'))

from  wiredtiger import wiredtiger_open

def main():

    home = "WT_TEST_0_0"
    backup_dir = "WT_TEST_0_0_backup"

    # Create a clean backup directory
    if os.path.exists(backup_dir):
        shutil.rmtree(backup_dir)
    os.makedirs(backup_dir)

    # Connect to the database and open a session
    conn = wiredtiger_open('WT_TEST_0_0', None)
    session = conn.open_session()

    backup_cursor = session.open_cursor('backup:', None)

    while backup_cursor.next() == 0:
        orig_file = f'{home}/{backup_cursor.get_key()}'
        backup_file = f'{backup_dir}/{backup_cursor.get_key()}'
        shutil.copyfile(orig_file, backup_file)

    backup_cursor.close()
    session.close()
    conn.close()

if __name__ == "__main__":
    main()
