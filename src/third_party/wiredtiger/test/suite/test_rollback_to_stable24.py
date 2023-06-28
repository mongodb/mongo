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

from rollback_to_stable_util import verify_rts_logs
import wttest
from wtscenario import make_scenarios

# test_rollback_to_stable24.py
# Exercise a recno-counting bug in column store.
#
# Prior to August 2021 a cell for which there's a pending stable update was counted (in the
# column-store RTS code) as having RLE count 1 regardless of what the actual count was.
#
# In order to exploit this we have to do janky things with timestamps, but I think they're
# allowable.
#
# Construct a cell with RLE count of 3 by writing 3 copies of aaaaaa at timestamp 10.
# Then at the next key write bbbbbb at timestamp 10 and cccccc at timestamp 50.
# Evict the page to reconcile it and produce the RLE cell.
#
# Then post an update to the first key of the RLE cell at timestamp 30 (to dddddd), and roll
# back to 40.
#
# Reading at 40, we should at that point see dddddd and two aaaaaa's followed by bbbbbb, but
# with the bad counting we get a key error on the second key.
#
# This happens because it goes to process key 4 but thinks it's on key 2; it finds that it
# needs to roll back the value it's looking at (the cccccc from timestamp 50) but because it
# thinks it's on key to it asks the history store for key 2 and finds nothing. (The bbbbbb
# from timestamp 10 is in the history store, but under key 4; there's nothing in the history
# store for key 2.) So it issues a tombstone, and issues it for key 2, so key 2 improperly
# disappears.
#
# Run this test on rows as well as columns to help make sure the test itself is valid (and
# stays so over time...)
#
# Don't run it on FLCS because FLCS doesn't do RLE encoding so there's no point.
class test_rollback_to_stable24(wttest.WiredTigerTestCase):
    conn_config = 'in_memory=false'

    key_format_values = [
        ('column', dict(key_format='r')),
        ('row_integer', dict(key_format='i')),
    ]

    scenarios = make_scenarios(key_format_values)

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RTS')
        self.addTearDownAction(verify_rts_logs)

    def conn_config(self):
        return 'verbose=(rts:5)'

    def test_rollback_to_stable24(self):
        # Create a table without logging.
        uri = "table:rollback_to_stable24"
        self.session.create(uri, 'key_format={},value_format=S'.format(self.key_format))

        # Pin oldest timestamp to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5))

        # Start stable timestamp at 5.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))

        value_a = "aaaaa" * 100
        value_b = "bbbbb" * 100
        value_c = "ccccc" * 100
        value_d = "ddddd" * 100

        s = self.conn.open_session()
        cursor = s.open_cursor(uri)

        # Write some keys at time 10.
        s.begin_transaction()
        cursor[1] = value_a
        cursor[2] = value_a
        cursor[3] = value_a
        cursor[4] = value_b
        s.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Update key 4 at time 50.
        s.begin_transaction()
        cursor[4] = value_c
        s.commit_transaction('commit_timestamp=' + self.timestamp_str(50))

        cursor.close()

        # Evict the page to force reconciliation.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        s.begin_transaction()
        # Search the key to evict it.
        v = evict_cursor[1]
        self.assertEqual(v, value_a)
        self.assertEqual(evict_cursor.reset(), 0)
        s.rollback_transaction()
        evict_cursor.close()

        # Now update key 1 at time 30.
        cursor = s.open_cursor(uri)
        s.begin_transaction()
        cursor[1] = value_d
        s.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        # Roll back to 40.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.conn.rollback_to_stable()

        # Now read at 40.
        cursor = s.open_cursor(uri)
        s.begin_transaction('read_timestamp=' + self.timestamp_str(40))
        self.assertEqual(cursor[1], value_d)
        self.assertEqual(cursor[2], value_a)
        self.assertEqual(cursor[3], value_a)
        self.assertEqual(cursor[4], value_b)
        s.rollback_transaction()
        cursor.close()
