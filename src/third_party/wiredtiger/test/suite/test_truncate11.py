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
# test_truncate11.py
#   Check for checkpoint not reading the deleted pages that are marked by
#   a fast-truncate which is not visible to the checkpoint.

import threading, time, wttest
from wtdataset import simple_key, simple_value
from wtscenario import make_scenarios
from wiredtiger import stat
from wtthread import checkpoint_thread

class test_truncate11(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB,statistics=(all),statistics_log=(json,on_close,wait=1),timing_stress_for_test=[checkpoint_slow]'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_truncate11(self):
        # Create a large table with lots of pages.
        uri = "table:test_truncate11"
        format = 'key_format={},value_format=S'.format(self.key_format)
        self.session.create(uri, 'allocation_size=512,leaf_page_max=512,' + format)

        cursor = self.session.open_cursor(uri)
        for i in range(1, 80000):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)
        cursor.close()

        valuea = "aaaaa" * 100

        # Force to disk.
        self.reopen_conn()

        # Set the oldest timestamp and the stable timestamp.
        self.conn.set_timestamp('oldest_timestamp='+ self.timestamp_str(100))
        self.conn.set_timestamp('stable_timestamp='+ self.timestamp_str(100))

        # Update a large number of records.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, 10):
            cursor[i] = valuea
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(120))
        cursor.close()

        # Create a checkpoint thread
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()

            # Wait for checkpoint to start before committing last transaction.
            ckpt_started = 0
            while not ckpt_started:
                time.sleep(1)
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_started = stat_cursor[stat.conn.txn_checkpoint_running][2]
                stat_cursor.close()

            # Start a transaction.
            self.session.begin_transaction()

            # Truncate a chunk.
            c1 = self.session.open_cursor(uri, None)
            c1.set_key(simple_key(c1, 20000))
            c2 = self.session.open_cursor(uri, None)
            c2.set_key(simple_key(c1, 40000))

            self.session.truncate(None, c1, c2, None)

            # Commit the transaction.
            self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(150))
            self.session.commit_transaction()

        finally:
            done.set()
            ckpt.join()

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        read_deleted = stat_cursor[stat.conn.cache_read_deleted][2]
        self.assertLess(read_deleted, 10)
        stat_cursor.close()

if __name__ == '__main__':
    wttest.run()
