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
# checkpoint:obsolete_data
# [END_TAGS]
#
# test_checkpoint08.py
# Test that the btree checkpoint is not skipped if there are obsolete pages.

import wttest
from wiredtiger import stat

class test_checkpoint08(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'

    def get_stat(self, uri):
        stat_uri = 'statistics:' + uri
        stat_cursor = self.session.open_cursor(stat_uri)
        val = stat_cursor[stat.dsrc.btree_clean_checkpoint_timer][2]
        stat_cursor.close()
        return val

    def test_checkpoint08(self):
        self.uri1 = 'table:ckpt08.1'
        self.file1 = 'file:ckpt08.1.wt'
        self.uri2 = 'table:ckpt08.2'
        self.file2 = 'file:ckpt08.2.wt'
        self.hsfile = 'file:WiredTigerHS.wt'
        self.session.create(self.uri1, 'key_format=i,value_format=i')
        self.session.create(self.uri2, 'key_format=i,value_format=i')

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Setup: Insert some data and checkpoint it. Then modify only
        # the data in the first table and checkpoint. Verify the clean skip
        # timer is not set for the modified table and is set for the clean one.
        c1 = self.session.open_cursor(self.uri1, None)
        c2 = self.session.open_cursor(self.uri2, None)

        self.session.begin_transaction()
        c1[1] = 1
        c2[1] = 1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        self.session.begin_transaction()
        c1[1] = 10
        c2[1] = 10
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(3))
        self.session.checkpoint(None)

        # Modify the both tables and reverify.
        self.session.begin_transaction()
        c1[3] = 3
        c2[3] = 3
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(4))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(4))
        self.session.checkpoint(None)

        val = self.get_stat(self.uri1)
        self.assertEqual(val, 0)
        val = self.get_stat(self.uri2)
        self.assertEqual(val, 0)
        hsval = self.get_stat(self.hsfile)
        self.assertNotEqual(hsval, 0)

        # Modify the both tables and reverify when oldest timestamp moved.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(4))
        self.session.begin_transaction()
        c1[4] = 4
        c2[4] = 4
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.session.checkpoint(None)

        val = self.get_stat(self.uri1)
        self.assertEqual(val, 0)
        val = self.get_stat(self.uri2)
        self.assertEqual(val, 0)
        hsval = self.get_stat(self.hsfile)
        self.assertEqual(hsval, 0)

        stat_cursor = self.session.open_cursor('statistics:file:WiredTigerHS.wt', None, None)
        obsolete_applied = stat_cursor[stat.dsrc.txn_checkpoint_obsolete_applied][2]
        self.assertEqual(obsolete_applied, 1)
        stat_cursor.close()

        c1.close()
        c2.close()
        self.session.close()

if __name__ == '__main__':
    wttest.run()
