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
from wiredtiger import stat

# test_eviction05.py
#
# Test that eviction max stats per database run are not reset after a checkpoint.
@wttest.skip_for_hook("disagg", "Fails due to evict a page.")
class test_eviction05(wttest.WiredTigerTestCase):

    def conn_config(self):
        config = 'cache_size=10MB,statistics=(all),statistics_log=(json,on_close,wait=1)'
        return config

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_eviction(self):
        uri = 'table:test_eviction05'

        # Create a table.
        self.session.create(uri, 'key_format=i,value_format=S')
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)

        # Insert a key 1 and commit the transaction.
        session2.begin_transaction()
        cursor2[1] = '20'
        session2.commit_transaction()
        cursor2.close()

        # Set the key to 1 in the evict the page.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        evict_cursor.set_key(1)
        self.assertEqual(evict_cursor.search(), 0)
        self.assertEqual(evict_cursor.reset(), 0)
        evict_cursor.close()

        self.assertGreater(self.get_stat(stat.conn.eviction_maximum_page_size), 0)
        self.assertGreater(self.get_stat(stat.conn.eviction_maximum_page_size_per_checkpoint), 0)

        # Run a checkpoint and verify that eviction max stats per database run are not reset.
        self.session.checkpoint()

        self.assertGreater(self.get_stat(stat.conn.eviction_maximum_page_size), 0)
        self.assertEqual(self.get_stat(stat.conn.eviction_maximum_page_size_per_checkpoint), 0)


if __name__ == '__main__':
    wttest.run()
