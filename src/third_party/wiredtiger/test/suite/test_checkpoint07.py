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
# test_checkpoint07.py
# Test that the checkpoints timing statistics are populated as expected.

import wttest
from wiredtiger import stat

class test_checkpoint07(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all)'

    def get_stat(self, uri):
        stat_uri = 'statistics:' + uri
        stat_cursor = self.session.open_cursor(stat_uri)
        val = stat_cursor[stat.dsrc.btree_clean_checkpoint_timer][2]
        stat_cursor.close()
        return val

    def test_checkpoint07(self):
        self.uri1 = 'table:ckpt05.1'
        self.file1 = 'file:ckpt05.1.wt'
        self.uri2 = 'table:ckpt05.2'
        self.file2 = 'file:ckpt05.2.wt'
        self.uri3 = 'table:ckpt05.3'
        self.file3 = 'file:ckpt05.3.wt'
        self.session.create(self.uri1, 'key_format=i,value_format=i')
        self.session.create(self.uri2, 'key_format=i,value_format=i')
        self.session.create(self.uri3, 'key_format=i,value_format=i')

        # Setup: Insert some data and checkpoint it. Then modify only
        # the data in the first table and checkpoint. Verify the clean skip
        # timer is not set for the modified table and is set for the clean one.
        c1 = self.session.open_cursor(self.uri1, None)
        c2 = self.session.open_cursor(self.uri2, None)
        c3 = self.session.open_cursor(self.uri3, None)
        c1[1] = 1
        c2[1] = 1
        c3[1] = 1
        self.session.checkpoint(None)
        c1[2] = 2
        self.session.checkpoint(None)
        val1 = self.get_stat(self.file1)
        self.assertEqual(val1, 0)
        val2 = self.get_stat(self.file2)
        self.assertNotEqual(val2, 0)
        val3 = self.get_stat(self.file3)
        self.assertNotEqual(val3, 0)
        # It is possible that we could span the second timer when processing table
        # two and table three during the checkpoint. If they're different check
        # they are within 1 second of each other.
        if val2 != val3:
            self.assertTrue(val2 == val3 - 1 or val3 == val2 - 1)

        # Now force a checkpoint on clean tables. No clean timer should be set.
        self.session.checkpoint('force=true')
        val = self.get_stat(self.uri1)
        self.assertEqual(val, 0)
        val = self.get_stat(self.uri2)
        self.assertEqual(val, 0)
        val = self.get_stat(self.uri3)
        self.assertEqual(val, 0)

        # Modify the first two tables and reverify all three.
        c1[3] = 3
        c2[3] = 3
        self.session.checkpoint(None)
        val = self.get_stat(self.uri1)
        self.assertEqual(val, 0)
        val = self.get_stat(self.uri2)
        self.assertEqual(val, 0)
        val = self.get_stat(self.uri3)
        self.assertNotEqual(val, 0)

        # Open a backup cursor. This will pin the most recent checkpoint.
        # Modify table 1 and checkpoint, then modify table 2 and checkpoint.
        # The open backup cursor will cause table 1 to get the smaller timer.
        backup_cursor = self.session.open_cursor('backup:', None, None)
        c1[4] = 4
        self.session.checkpoint(None)
        val = self.get_stat(self.uri1)
        self.assertEqual(val, 0)

        c2[4] = 4
        self.session.checkpoint(None)
        val2 = self.get_stat(self.uri2)
        self.assertEqual(val2, 0)

        val1 = self.get_stat(self.uri1)
        val3 = self.get_stat(self.uri3)
        # Assert table 1 does not have the forever timer value, but it is set.
        # This assumes table 3 has the forever value.
        self.assertNotEqual(val1, 0)
        self.assertNotEqual(val3, 0)
        self.assertLess(val1, val3)
        # Save the old forever value from table 3.
        oldval3 = val3

        # Force a checkpoint while the backup cursor is open. Then write again
        # to table 2. Since table 1 and table 3 are clean again, this should
        # force both table 1 and table 3 to have the smaller timer.
        self.session.checkpoint('force=true')
        val1 = self.get_stat(self.uri1)
        val3 = self.get_stat(self.uri3)
        self.assertEqual(val1, 0)
        self.assertEqual(val3, 0)
        c2[5] = 5
        self.session.checkpoint(None)
        val2 = self.get_stat(self.uri2)
        self.assertEqual(val2, 0)

        val1 = self.get_stat(self.uri1)
        val3 = self.get_stat(self.uri3)
        self.assertNotEqual(val1, 0)
        self.assertNotEqual(val3, 0)
        self.assertLess(val3, oldval3)
        # It is possible that we could span the second timer when processing table
        # two and table three during the checkpoint. If they're different check
        # they are within 1 second of each other.
        if val1 != val3:
            self.assertTrue(val1 == val3 - 1 or val3 == val1 - 1)

        backup_cursor.close()

        # Repeat the sequence of forcing a checkpoint and then modifying after
        # closing the backup cursor to check that both tables are now marked
        # with the forever value.
        self.session.checkpoint('force=true')
        val1 = self.get_stat(self.uri1)
        val3 = self.get_stat(self.uri3)
        self.assertEqual(val1, 0)
        self.assertEqual(val3, 0)
        c2[6] = 6
        self.session.checkpoint(None)
        val2 = self.get_stat(self.uri2)
        self.assertEqual(val2, 0)

        val1 = self.get_stat(self.uri1)
        val3 = self.get_stat(self.uri3)
        self.assertNotEqual(val1, 0)
        self.assertNotEqual(val3, 0)
        # It is possible that we could span the second timer when processing table
        # two and table three during the checkpoint. If they're different check
        # they are within 1 second of each other.
        if val1 != val3:
            self.assertTrue(val1 == val3 - 1 or val3 == val1 - 1)
        self.assertEqual(val3, oldval3)

        self.session.close()

if __name__ == '__main__':
    wttest.run()
