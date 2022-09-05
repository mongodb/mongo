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

from helper import simulate_crash_restart
from test_rollback_to_stable01 import test_rollback_to_stable_base
from wiredtiger import stat, WT_NOTFOUND
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable34.py
# Test interaction between fast-delete and RTS.
class test_rollback_to_stable34(test_rollback_to_stable_base):
    session_config = 'isolation=snapshot'
    conn_config = 'cache_size=50MB,statistics=(all),log=(enabled=false)'

    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('column_fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('integer_row', dict(key_format='i', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True)),
    ]
    second_checkpoint_values = [
        ('second_checkpoint', dict(second_checkpoint=True)),
        ('no_second_checkpoint', dict(second_checkpoint=False)),
    ]
    rollback_modes = [
        ('runtime', dict(crash=False)),
        ('recovery', dict(crash=True)),
    ]

    scenarios = make_scenarios(format_values, prepare_values, second_checkpoint_values,
        rollback_modes)

    # Make all the values different so it's easier to see what happens if ranges go missing.
    def mkdata(self, basevalue, i):
        if self.value_format == '8t':
            return basevalue
        return basevalue + str(i)

    def evict(self, ds, lo, hi, basevalue):
        evict_cursor = self.session.open_cursor(ds.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()

        # Evict every 3rd key to make sure we get all the pages but not write them out
        # over and over again any more than necessary. FUTURE: improve this to evict
        # each page once when we get a suitable interface for that.
        for i in range(lo, hi, 3):
            evict_cursor.set_key(ds.key(i))
            self.assertEquals(evict_cursor.search(), 0)
            self.assertEquals(evict_cursor.get_value(), self.mkdata(basevalue, i))
            evict_cursor.reset()
        self.session.rollback_transaction()
        evict_cursor.close()

    # Call this checkx to distinguish it from the parent class's default check().
    def checkx(self, ds, nrows, read_ts, basevalue):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = self.session.open_cursor(ds.uri)
        i = 1
        for k, v in cursor:
            self.assertEqual(v, self.mkdata(basevalue, i))
            self.assertEqual(k, ds.key(i))
            i += 1
        self.session.commit_transaction()
        self.assertEqual(i, nrows + 1)
        cursor.close()

    def test_rollback_to_stable(self):
        # RTS will fail if there are uncommitted prepared transactions, so skip tests of prepare
        # with a runtime call to RTS, that doesn't add useful testing scenarios.
        if self.prepare and not self.crash:
            return

        nrows = 10000

        # Create a table without logging.
        uri = "table:rollback_to_stable34"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)' + self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        cursor = self.session.open_cursor(uri)

        # Write some baseline data out at time 20.
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = self.mkdata(valuea, i)
            # Make a new transaction every 97 keys so the transactions don't get huge.
            if i % 97 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Write some more data out at time 30.
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = self.mkdata(valueb, i)
            # Make a new transaction every 97 keys so the transactions don't get huge.
            if i % 97 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        cursor.close()

        # Evict the lot.
        self.evict(ds, 1, nrows + 1, valueb)

        # Move stable to 25 (after the baseline data).
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(25))

        # Checkpoint.
        self.session.checkpoint()

        # Now fast-delete the lot at time 35.
        # Use a separate session for this so that if we leave the truncate prepared it
        # doesn't obstruct the rest of the test.
        session2 = self.conn.open_session()
        session2.begin_transaction()
        lo_cursor = session2.open_cursor(uri)
        lo_cursor.set_key(ds.key(nrows // 2 + 1))
        hi_cursor = session2.open_cursor(uri)
        hi_cursor.set_key(ds.key(nrows + 1))
        session2.truncate(None, lo_cursor, hi_cursor, None)
        if self.prepare:
            session2.prepare_transaction('prepare_timestamp=' + self.timestamp_str(35))
        else:
            session2.commit_transaction('commit_timestamp=' + self.timestamp_str(35))
        hi_cursor.close()
        lo_cursor.close()

        # Check stats to make sure we fast-deleted at least one page.
        # (Except for FLCS, where it's not supported and we should fast-delete zero pages.)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        if self.value_format == '8t':
            self.assertEqual(fastdelete_pages, 0)
        else:
            self.assertGreater(fastdelete_pages, 0)

        if self.second_checkpoint:
            # Checkpoint again with the deletion.
            self.session.checkpoint()

        # Roll back, either via crashing or by explicit RTS.
        if self.crash:
            simulate_crash_restart(self, ".", "RESTART")
        else:
            self.conn.rollback_to_stable()

        # We should see the original data at read-ts 20 and 30.
        self.checkx(ds, nrows, 20, valuea)
        self.checkx(ds, nrows, 30, valuea)
