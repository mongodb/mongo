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
from helper import simulate_crash_restart
from wiredtiger import stat, WiredTigerError, wiredtiger_strerror, WT_NOTFOUND, WT_ROLLBACK
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_truncate12.py
#
# Make sure that transaction IDs on truncates are handled properly after recovery,
# even if the truncate information is loaded during recovery and stays in cache.
#
# This version uses timestamps and no logging.

class test_truncate12(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'
    session_config = 'isolation=snapshot'

    # Hook to run using remove instead of truncate for reference. This should not alter the
    # behavior... but may if things are broken. Disable the reference version by default as it's
    # only useful when investigating behavior changes. This list is first in the make_scenarios
    # call so the additional cases don't change the scenario numbering.
    trunc_values = [
        ('truncate', dict(trunc_with_remove=False)),
        #('remove', dict(trunc_with_remove=True)),
    ]
    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('column_fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('integer_row', dict(key_format='i', value_format='S', extraconfig='')),
    ]
    scenarios = make_scenarios(trunc_values, format_values)

    def truncate(self, uri, make_key, keynum1, keynum2):
        if self.trunc_with_remove:
            cursor = self.session.open_cursor(uri)
            err = 0
            for k in range(keynum1, keynum2 + 1):
                cursor.set_key(k)
                try:
                    err = cursor.remove()
                except WiredTigerError as e:
                    if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                        err = WT_ROLLBACK
                    else:
                        raise e
                if err != 0:
                    break
            cursor.close()
        else:
            lo_cursor = self.session.open_cursor(uri)
            hi_cursor = self.session.open_cursor(uri)
            lo_cursor.set_key(make_key(keynum1))
            hi_cursor.set_key(make_key(keynum2))
            try:
                err = self.session.truncate(None, lo_cursor, hi_cursor, None)
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                    err = WT_ROLLBACK
                else:
                    raise e
            lo_cursor.close()
            hi_cursor.close()
        return err

    def check(self, ds, cursor, value, keep, nrows):
        def expect(lo, hi):
            for i in range(lo, hi):
                self.assertEqual(cursor[ds.key(i)], value)
        def expectNone(lo, hi):
            for i in range(lo, hi):
                cursor.set_key(ds.key(i))
                if self.value_format == '8t' and i <= nrows:
                    # In FLCS, deleted values read back as zero. Except past end-of-table.
                    self.assertEqual(cursor.search(), 0)
                    self.assertEqual(cursor.get_value(), 0)
                else:
                    self.assertEqual(cursor.search(), WT_NOTFOUND)

        # Expect 1..keep+1 to have values, and the rest not.
        expect(1, keep + 1)
        expectNone(keep + 1, nrows + 1)

    def test_truncate12(self):
        nrows = 10000
        keep_rows = 5

        # Create two tables.
        uri1 = "table:truncate12a"
        uri2 = "table:truncate12b"
        ds1 = SimpleDataSet(
            self, uri1, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds2 = SimpleDataSet(
            self, uri2, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds1.populate()
        ds2.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_small = 42
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_small = "***"

        # Pin oldest and stable timestamps to 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write some baseline data to table 1 at time 10.
        cursor1 = self.session.open_cursor(ds1.uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor1[ds1.key(i)] = value_a
            if i % 480 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor1.close()

        # Mark it stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Reopen the connection so nothing is in memory and we can fast-truncate.
        self.reopen_conn()

        # Write a lot of rubbish to table 2 to cycle through transaction IDs.
        # Do this at time 20.
        cursor2 = self.session.open_cursor(ds2.uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor2[ds2.key(i)] = value_small
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Truncate all of table 1 except for the first few keys.
        # Commit the truncate at time 30.
        self.session.begin_transaction()
        err = self.truncate(ds1.uri, ds1.key, keep_rows + 1, nrows)
        self.assertEqual(err, 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Make sure we did at least one fast-delete. (Unless we specifically didn't want to,
        # or running on FLCS where it isn't supported.)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        if self.value_format == '8t' or self.trunc_with_remove:
            self.assertEqual(fastdelete_pages, 0)
        else:
            self.assertGreater(fastdelete_pages, 0)

        # Now update the values we left behind, at time 40.
        cursor1 = self.session.open_cursor(ds1.uri)
        self.session.begin_transaction()
        for i in range(1, keep_rows + 1):
            cursor1[ds1.key(i)] = value_b
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(40))
        cursor1.close()

        # Doing that should not have instantiated any deleted pages.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        read_deleted = stat_cursor[stat.conn.cache_read_deleted][2]
        self.assertEqual(read_deleted, 0)
        stat_cursor.close()

        # Advance stable to 35. We'll be rolling back the updated keys but not the truncate.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(35))

        # Checkpoint so the truncate gets written out. We're interested in transaction ID
        # handling across database runs, so we need it all out on disk.
        self.session.checkpoint('name=pointy')

        # Now crash. It's important to do this (and not just reopen) so the unstable material
        # gets rolled back during recovery in startup and not by the shutdown-time RTS.
        simulate_crash_restart(self, ".", "RESTART")

        # Recovery should not have instantiated any deleted pages. But it should have loaded
        # the first internal page, which should contain at least a few deleted pages.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        read_deleted = stat_cursor[stat.conn.cache_read_deleted][2]
        self.assertEqual(read_deleted, 0)
        stat_cursor.close()

        # Validate the data. Because we cranked forward the transaction IDs, the truncate
        # transactions should have large transaction IDs and if we mishandle the write
        # generation because the internal pages were loaded during RTS, the truncates won't
        # be visible.
        cursor1 = self.session.open_cursor(ds1.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(50))
        self.check(ds1, cursor1, value_a, keep_rows, nrows)
        self.session.rollback_transaction()
        cursor1.close()

        # For good measure, validate the data in the checkpoint we wrote as well.
        # (This isn't part of the primary goal of this test but is fast and doesn't hurt.)
        pointy_cursor = self.session.open_cursor(ds1.uri, None, "checkpoint=pointy")
        self.check(ds1, pointy_cursor, value_a, keep_rows, nrows)
        pointy_cursor.close()

if __name__ == '__main__':
    wttest.run()
