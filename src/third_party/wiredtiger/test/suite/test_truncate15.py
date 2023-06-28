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
from wiredtiger import stat, WiredTigerError, wiredtiger_strerror, WT_ROLLBACK
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_truncate15.py
#
# Check that readonly database reading fast truncated pages doesn't lead to cache stuck.

class test_truncate15(wttest.WiredTigerTestCase):
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
        # Do not run against FLCS until/unless it gets fast-truncate support.
        # The issue at hand is specific to fast-truncate pages and is not relevant to slow-truncate.
        #('column_fix', dict(key_format='r', value_format='8t',
        #    extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
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

    def check(self, uri, make_key, nrows, nzeros, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        seen = 0
        zseen = 0
        for k, v in cursor:
            if self.value_format == '8t' and v == 0:
                zseen += 1
            else:
                self.assertEqual(v, value)
                seen += 1
        self.assertEqual(seen, nrows)
        self.assertEqual(zseen, nzeros if self.value_format == '8t' else 0)
        self.session.rollback_transaction()
        cursor.close()

    def evict_cursor(self, uri, ds, nrows, ts):
        s = self.conn.open_session()
        s.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        # Configure debug behavior on a cursor to evict the page positioned on when reset is called.
        evict_cursor = s.open_cursor(uri, None, "debug=(release_evict)")
        for i in range(1, nrows + 1):
            evict_cursor.set_key(ds.key(i))
            evict_cursor.search()
            evict_cursor.reset()
        s.rollback_transaction()
        evict_cursor.close()

    def test_truncate15(self):
        # Note: 50000 is not large enough to trigger the problem.
        nrows = 100000

        uri = "table:truncate15"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)' + self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
        else:
            value_a = "aaaaa" * 500

        # Pin oldest and stable timestamps to 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write a bunch of data at time 10.
        cursor = self.session.open_cursor(ds.uri)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_a
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Mark it stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Reopen the connection so nothing is in memory and we can fast-truncate.
        self.reopen_conn()

        # Truncate the data at time 25, but prepare at 20 and make durable 30.
        self.session.begin_transaction()
        err = self.truncate(ds.uri, ds.key, nrows // 4 + 1, nrows // 4 + nrows // 2)
        self.assertEqual(err, 0)
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(25))
        self.session.commit_transaction('durable_timestamp=' + self.timestamp_str(30))

        # Make sure we did at least one fast-delete. For FLCS, there's no fast-delete
        # support, so assert we didn't.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        if self.value_format == '8t':
            self.assertEqual(fastdelete_pages, 0)
        else:
            self.assertGreater(fastdelete_pages, 0)

        # Advance stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.session.checkpoint()

        # Reopen the connection so nothing is in memory.
        self.reopen_conn(".", "cache_size=1MB,eviction_dirty_target=90,eviction_dirty_trigger=100" +
            ",eviction_updates_target=90,eviction_updates_trigger=100,readonly=true")

        # Validate the data.
        try:
            # At time 10 we should see all value_a.
            self.check(ds.uri, ds.key, nrows, 0, value_a, 10)
            #self.evict_cursor(ds.uri, ds, nrows, 10)

            # At time 20 we should still see all value_a.
            self.check(ds.uri, ds.key, nrows, 0, value_a, 20)
            #self.evict_cursor(ds.uri, ds, nrows, 20)

            # At time 25 we should still see half value_a, and for FLCS, half zeros.
            self.check(ds.uri, ds.key, nrows // 2, nrows // 2, value_a, 25)
            #self.evict_cursor(ds.uri, ds, nrows // 2, 25)

            # At time 30 we should also see half value_a, and for FLCS, half zeros.
            self.check(ds.uri, ds.key, nrows // 2, nrows // 2, value_a, 30)
            #self.evict_cursor(ds.uri, ds, nrows // 2, 30)
        except WiredTigerError as e:
            # If we get WT_ROLLBACK while reading, assume it's because we overflowed the
            # cache, and fail. (If we don't trap this explicitly, the test harness retries
            # the whole test, which is not what we want.)
            if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                self.assertTrue(False)
            else:
                raise e

if __name__ == '__main__':
    wttest.run()

