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

import threading
from rollback_to_stable_util import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wtthread import checkpoint_thread

# test_prepare21.py
# Test prepare rollback doesn't crash because of triggering out of order fix.
class test_prepare21(test_rollback_to_stable_base):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        config = 'cache_size=10MB,statistics=(all),timing_stress_for_test=[history_store_checkpoint_delay]'
        return config

    def evict_cursor(self, uri, nrows):
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction("ignore_prepare=true")
        for i in range (1, nrows + 1):
            evict_cursor.set_key(i)
            evict_cursor.search()
            evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

    def test_prepare_rollback(self):
        nrows = 10

        # Create a table.
        uri = "table:prepare21"
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.value_format == '8t':
             value_a = 97
             value_b = 98
             value_c = 99
             value_d = 100
        else:
             value_a = "aaaaa" * 100
             value_b = "bbbbb" * 100
             value_c = "ccccc" * 100
             value_d = "ddddd" * 100

        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        self.large_updates(uri, value_a, ds, nrows, False, 20)
        self.large_updates(uri, value_b, ds, nrows, False, 30)
        self.large_removes(uri, ds, nrows, False, 40)

        prepare_session = self.conn.open_session()
        prepare_session.begin_transaction()
        cursor = prepare_session.open_cursor(uri)
        for i in range (1, nrows + 1):
            cursor[i] = value_c
        cursor.close()
        prepare_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(50))

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_b, uri, nrows, None, 30)

        self.evict_cursor(uri, nrows)

        # Pin stable to timestamp 40.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Rollback the prepared update
        prepare_session.rollback_transaction()
        self.large_updates(uri, value_d, ds, nrows, False, 60)

        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        try:
            ckpt.start()

            # Wait for checkpoint to start before committing last transaction.
            ckpt_started = 0
            while not ckpt_started:
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_started = stat_cursor[stat.conn.txn_checkpoint_running][2]
                stat_cursor.close()

            self.evict_cursor(uri, nrows)
        finally:
            done.set()
            ckpt.join()

        # Verify data is visible and correct.
        self.check(value_a, uri, nrows, None, 20)
        self.check(value_b, uri, nrows, None, 30)
        self.check(value_d, uri, nrows, None, 60)

if __name__ == '__main__':
    wttest.run()
