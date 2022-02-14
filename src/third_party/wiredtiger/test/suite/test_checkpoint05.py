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
# test_checkpoint05.py
# Verify that we don't accumulate a lot of checkpoints while a backup
# cursor is open. WiredTiger checkpoints created after the backup cursor
# should get deleted as usual.

import time
import wttest

class test_checkpoint05(wttest.WiredTigerTestCase):
    conn_config = 'create,cache_size=100MB,log=(enabled=true,file_max=100K,remove=false)'

    def count_checkpoints(self):
        metadata_cursor = self.session.open_cursor('metadata:', None, None)

        nckpt = 0
        while metadata_cursor.next() == 0:
            key = metadata_cursor.get_key()
            value = metadata_cursor[key]
            nckpt += value.count("WiredTigerCheckpoint")
        metadata_cursor.close()
        return nckpt

    def test_checkpoints_during_backup(self):
        self.uri = 'table:ckpt05'
        self.session.create(self.uri, 'key_format=i,value_format=i')

        # Setup: Insert some data and checkpoint it
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(16):
            cursor[i] = i
        self.session.checkpoint(None)

        # Create backup and check how many checkpoints we have.
        backup_cursor = self.session.open_cursor('backup:', None, None)
        initial_count = self.count_checkpoints()

        # Checkpoints created immediately after a backup cursor may get pinned.
        # Pause to avoid this.
        time.sleep(2)

        # Take a bunch of checkpoints.
        for i in range (50):
            self.session.checkpoint('force=true')
        cursor.close()

        # There may be a few more checkpoints than when we opened the
        # backup cursor, but not too many more.  The factor of three
        # is generous.  But if WT isn't deleting checkpoints there would
        # be about 30x more checkpoints here.
        final_count = self.count_checkpoints()
        self.assertTrue (final_count < initial_count * 3)

        self.session.close()

if __name__ == '__main__':
    wttest.run()
