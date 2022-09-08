
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
# [TEST_TAGS]
# checkpoint:recovery
# [END_TAGS]

import wttest
import os, shutil

# test_bug029.py
#
# Test that WT correctly propogates the most recent checkpoint time
# across restarts. We validate this by reproducing the original bug
# from WT-9457: frequent checkpoints pushed the checkpoint clock time
# into the future such that immediately after a restart a backup could
# see its checkpoint deleted out from under it. The result was fatal
# read errors when restoring the backup.

class test_bug029(wttest.WiredTigerTestCase):
    conn_config = ("cache_size=50MB")
    uri = "table:test_bug029"
    bigvalue = "WiredTiger" * 100
    backup_dir = "backup_dir"

    def add_data(self, uri, start, count):
        cursor = self.session.open_cursor(uri, None)
        for i in range(start, start + count): 
            cursor[i] = self.bigvalue
        cursor.close()

    def test_bug029(self):
        # Create and populate table
        self.session.create(self.uri, "key_format=i,value_format=S")
        self.add_data(self.uri, 0, 2000)
        self.session.checkpoint()

        # Force the checkpoint time forward with a lot of quick checkpoints.
        for i in range(100):
            self.session.checkpoint("force=1")

        # Add more data and checkpoint again. This creates a bunch of pages
        # in the final checkpoint that can be deleted and reused if we hit
        # the bug.
        self.add_data(self.uri, 2000, 2000)
        self.session.checkpoint()

        # Shutdown and reopen.
        self.reopen_conn()

        self.add_data(self.uri, 0, 100)

        # Open a backup cursor and force a few checkpoints. This will allow
        # WT to delete older checkpoints, but as long as the backup cursor
        # is open, it shouldn't delete the backup checkpoint---unless we hit
        # the bug.
        backup_cursor = self.session.open_cursor('backup:')

        for i in range(10):
            self.session.checkpoint("force=1")

        # Write and checkpoint a bunch of data. If we erroneously deleted our 
        # backup checkpoint this should overwrite some of that checkpoint's
        # blocks.
        self.add_data(self.uri, 1000, 2000)
        self.session.checkpoint()

        # Now do the backup.
        os.mkdir(self.backup_dir)
        while True:
            ret = backup_cursor.next()
            if ret != 0:
                break
            shutil.copy(backup_cursor.get_key(), self.backup_dir)
        backup_cursor.close()

        # Open the backup and read data. If the backup snapshot was corrupted
        # we will panic and die here.
        backup_conn = self.wiredtiger_open(self.backup_dir, self.conn_config)
        session = backup_conn.open_session()
        cur1 = session.open_cursor(self.uri)
        for i in range(0, 4000, 10):
            self.assertEqual(cur1[i], self.bigvalue)

if __name__ == '__main__':
    wttest.run()
