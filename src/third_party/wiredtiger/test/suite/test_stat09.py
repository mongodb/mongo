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

import random
import wttest

# test_stat09.py
#    Check oldest active read timestamp statistic
class test_stat09(wttest.WiredTigerTestCase):
    tablename = 'test_stat09'
    uri = 'table:' + tablename
    conn_config = 'statistics=(all)'

    # Check the oldest active read statistic to be at the expected values
    def check_stat_oldest_read(self, statcursor, expected_oldest, all_committed):
        self.check_stats(statcursor, expected_oldest,
            'transaction: transaction read timestamp of the oldest active reader')

        # If the active oldest timestamp is 0, it implies there are no active readers,
        # the pinned range because of them is expected to be 0 in that case
        if expected_oldest == 0:
            expected_pinned = 0
        else:
            expected_pinned = all_committed - expected_oldest
        self.check_stats(statcursor, expected_pinned,
            'transaction: transaction range of timestamps pinned by the oldest '
            'active read timestamp')

    # Do a quick check of the entries in the stats cursor, the "lookfor"
    # string should appear with the exact val of "expected_val".
    def check_stats(self, statcursor, expected_val, lookfor):
        # Reset the cursor, we're called multiple times.
        statcursor.reset()

        found = False
        foundval = 0
        for id, desc, valstr, val in statcursor:
            if desc == lookfor:
                found = True
                foundval = val
                self.printVerbose(2, '  stat: \'' + desc + '\', \'' +
                    valstr + '\', ' + str(val))
                break

        self.assertTrue(found, 'in stats, did not see: ' + lookfor)
        self.assertTrue(foundval == expected_val)

    def test_oldest_active_read(self):
        self.session.create(self.uri, 'key_format=i,value_format=i')
        c = self.session.open_cursor(self.uri)

        # Insert some data: keys 1..100 each with timestamp=key, in some order
        commit_range = 100
        orig_keys = list(range(1, commit_range + 1))
        keys = orig_keys[:]
        random.shuffle(keys)

        for k in keys:
            self.session.begin_transaction()
            c[k] = 1
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(k))

        # Create a cursor on statistics that we can use repeatedly
        allstat_cursor = self.session.open_cursor('statistics:', None, None)

        # There being no active reader, the corresponding statistic should be 0
        self.check_stat_oldest_read(allstat_cursor, 0, commit_range)

        # Introduce multiple transactions with varying read_timestamp
        s1 = self.conn.open_session()
        s1.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        s2 = self.conn.open_session()
        s2.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        s3 = self.conn.open_session()
        s3.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        s4 = self.conn.open_session()
        s4.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        s5 = self.conn.open_session()
        s5.begin_transaction('read_timestamp=' + self.timestamp_str(50))

        # Check oldest reader
        self.check_stat_oldest_read(allstat_cursor, 10, commit_range)

        # Close the oldest reader and check again
        s1.commit_transaction()
        self.check_stat_oldest_read(allstat_cursor, 20, commit_range)

        # Set and advance the oldest timestamp, it should be ignored for
        # determining the oldest active read.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5))
        self.check_stat_oldest_read(allstat_cursor, 20, commit_range)

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(30))
        self.check_stat_oldest_read(allstat_cursor, 20, commit_range)

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(150))
        self.check_stat_oldest_read(allstat_cursor, 20, commit_range)

        # Move the commit timestamp and check again
        commit_range = 200
        s2.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_range))
        self.check_stat_oldest_read(allstat_cursor, 30, commit_range)

        # Close all the readers and check the oldest reader, it should be back to 0
        s3.commit_transaction()
        s4.commit_transaction()
        s5.commit_transaction()
        self.check_stat_oldest_read(allstat_cursor, 0, commit_range)

if __name__ == '__main__':
    wttest.run()
