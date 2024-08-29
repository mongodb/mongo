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

import wiredtiger, wttest
from wiredtiger import stat, WiredTigerError

# test_scrub_eviction_prepare.py
#
# Test to do the following steps.
# 1. Prepare an update with one key (key-2)
# 2. Commit an update with another key(key-1)
# 3, Make sure both the keys are on the same page.
# 4. Set the key to full update and evict the page with release_evict
# 5. Read the page back in memory
# 6. Checkpoint
# 7. Repeat steps 5,6 and validate that the page read back into memory should
#    not be reconciled everytime with the help of btree stat.
@wttest.skip_for_hook("tiered", "Fails with tiered storage")
class test_scrub_eviction_prepare(wttest.WiredTigerTestCase):

    def conn_config(self):
        config = 'cache_size=100MB,statistics=(all),statistics_log=(json,on_close,wait=1)'
        return config

    def get_stats(self, uri):
        stat_cursor = self.session.open_cursor('statistics:' + uri)
        btree_ckpt_pages_rec = stat_cursor[stat.dsrc.btree_checkpoint_pages_reconciled][2]
        stat_cursor.close()
        return btree_ckpt_pages_rec

    def read_key(self, uri):
        cur2 = self.session.open_cursor(uri)
        cur2.set_key(2)
        self.assertEqual(cur2.search(), 0)
        cur2.close()

    def test_scrub_eviction_prepare(self):
        uri = 'table:test_scrub_eviction_prepare'

        # Create a table.
        self.session.create(uri, 'key_format=i,value_format=S')
        session2 = self.conn.open_session()
        session3 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)

        # Insert a key 2 and commit the transaction.
        session2.begin_transaction()
        cursor2[2] = '20'
        session2.commit_transaction()

        # Insert a key 1 and prepare the transaction.
        session3.begin_transaction()
        cursor2[1] = '10'
        session3.prepare_transaction('prepare_timestamp=10')

        # Leaving a cursor open after updates can affect the release_evict cursor's
        # behavior. It might not guarantee eviction because the open cursor pins the
        # page in the cache. Therefore, always close the cursor explicitly before using.
        cursor2.close()

        # Set the key to 2(to avoid prepare conflict if the key is set to 1) in the evict
        # cursor and evict the page which has both the keys 1 and 2.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        evict_cursor.set_key(2)
        self.assertEqual(evict_cursor.search(), 0)
        self.assertEqual(evict_cursor.reset(), 0)
        evict_cursor.close()

        self.session.checkpoint()
        self.assertEqual(1, self.get_stats(uri))

        # Read the key 2 to avoid prepared conflict, this will bring back the page
        # that has both the keys 1 & 2 into the memory.
        self.read_key(uri)
        self.session.checkpoint()
        # The page with prepared update should not be reconciled again.
        self.assertEqual(1, self.get_stats(uri))

        # Read the key 2 to avoid prepared conflict, this will bring back the page
        # that has both the keys 1 & 2 into the memory.
        self.read_key(uri)
        self.session.checkpoint()
        # The page with prepared update should not be reconciled again.
        self.assertEqual(1, self.get_stats(uri))

if __name__ == '__main__':
    wttest.run()
