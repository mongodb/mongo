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

import wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_hs05.py
# Verify hs_score reflects cache pressure due to history
# even if we're not yet actively pushing into the history store file.
class test_hs05(wttest.WiredTigerTestCase):
    # Force a small cache, but disable eviction of dirty pages until the cache is full.
    conn_config = 'cache_size=50MB,statistics=(fast),'
    conn_config += 'eviction_trigger=100,'
    conn_config += 'eviction_dirty_target=95,eviction_dirty_trigger=100,'
    conn_config += 'eviction_updates_target=95,eviction_updates_trigger=100'
    session_config = 'isolation=snapshot'
    stable = 1
    key_format_values = [
        ('column', dict(key_format='r')),
        ('integer-row', dict(key_format='i')),
        ('string-row', dict(key_format='S'))
    ]
    scenarios = make_scenarios(key_format_values)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def large_updates(self, session, uri, value, ds, nrows, nops):
        # Update a large number of records, we'll hang if the history store table
        # isn't doing its thing.
        cursor = session.open_cursor(uri)
        score_start = self.get_stat(stat.conn.cache_hs_score)
        for i in range(nrows + 1, nrows + nops + 1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.stable + i))
        cursor.close()
        score_end = self.get_stat(stat.conn.cache_hs_score)
        score_diff = score_end - score_start
        self.pr("After large updates score start: " + str(score_start))
        self.pr("After large updates score end: " + str(score_end))
        self.pr("After large updates history store score diff: " + str(score_diff))

    def test_checkpoint_hs_reads(self):
        # Create a small table.
        uri = "table:test_hs05"
        nrows = 100
        ds = SimpleDataSet(self, uri, nrows, key_format=self.key_format, value_format='u')
        ds.populate()
        bigvalue = b"aaaaa" * 100

        # Initially load huge data.
        # Add 10000 items that have a 500b value that is about 50Mb that
        # is the entire cache. Then checkpoint so that none is required
        # to stay in cache.
        cursor = self.session.open_cursor(uri)
        for i in range(1, 10000):
            cursor[ds.key(nrows + i)] = bigvalue
        cursor.close()
        self.session.checkpoint()

        # Pin the oldest timestamp so that all history has to stay.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        # Loop a couple times, partly filling the cache but not
        # overfilling it to see the history store score value change
        # even if the history store is not yet in use.
        #
        # Use smaller values, 50 bytes and fill 8 times, under full cache.
        valstr='abcdefghijklmnopqrstuvwxyz'
        loop_start = self.get_stat(stat.conn.cache_hs_score)
        for i in range(1, 9):
            bigvalue2 = valstr[i].encode() * 50
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.stable))
            entries_start = self.get_stat(stat.conn.cache_hs_insert)
            score_start = self.get_stat(stat.conn.cache_hs_score)
            self.pr("Update iteration: " + str(i) + " Value: " + str(bigvalue2))
            self.pr("Update iteration: " + str(i) + " Score: " + str(score_start))
            self.large_updates(self.session, uri, bigvalue2, ds, nrows, nrows)
            self.stable += nrows
            score_end = self.get_stat(stat.conn.cache_hs_score)
            entries_end = self.get_stat(stat.conn.cache_hs_insert)
            # We expect to see the history store score increase but not writing
            # any new entries to the history store.
            self.assertGreaterEqual(score_end, score_start)
            self.assertEqual(entries_end, entries_start)

        # While each iteration may or may not increase the score, we expect the
        # score to have strictly increased from before the loop started.
        loop_end = self.get_stat(stat.conn.cache_hs_score)
        self.assertGreater(loop_end, loop_start)

        # Now move oldest timestamp forward and insert a couple large updates
        # but we should see the score drop because we allowed the history to move.
        # By moving the oldest after updating we should see the score drop
        # to zero.
        score_start = loop_end
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.stable))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.stable))
        for i in range(9, 11):
            bigvalue2 = valstr[i].encode() * 50
            self.pr("Update iteration with oldest: " + str(i) + " Value: " + str(bigvalue2))
            self.large_updates(self.session, uri, bigvalue2, ds, nrows, nrows)
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.stable))
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(self.stable))
            self.stable += nrows
        score_end = self.get_stat(stat.conn.cache_hs_score)
        self.assertLess(score_end, score_start)
        self.assertEqual(score_end, 0)

if __name__ == '__main__':
    wttest.run()
