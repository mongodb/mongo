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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_truncate06.py
#
# Test timestamped truncate of timestamped data changes in the presence of older history.
# Sixteen scenarios per table type cover:
#   - whether the changes are updates or removes;
#   - whether we evict all the pages before truncating;
#   - whether we checkpoint before truncating;
#   - whether the truncate notionally happens before (conflicting with) the changes.

class test_truncate06(wttest.WiredTigerTestCase):
    conn_config = ''

    # Hook to run using remove instead of truncate for reference. This should not alter the
    # behavior... but may if things are broken. Disable the reference version by default as it's
    # only useful when investigating behavior changes. This is first in the make_scenarios call
    # so the additional cases don't change the scenario numbering.
    trunc_values = [
        ('truncate', dict(trunc_with_remove=False)),
        #('remove', dict(trunc_with_remove=True)),
    ]

    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('column_fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('row_integer', dict(key_format='i', value_format='S', extraconfig='')),
    ]
    munge_values = [
        ('update', dict(munge_with_update=True)),
        ('remove', dict(munge_with_update=False)),
    ]
    # Try both with and without evicting the pages before truncating (which allows the
    # fast-delete code to run).
    eviction_values = [
        ('eviction', dict(do_evict=True)),
        ('no-eviction', dict(do_evict=False)),
    ]
    # Also try both with and without an intermediate checkpoint.
    checkpoint_values = [
        ('checkpoint', dict(do_checkpoint=True)),
        ('no-checkpoint', dict(do_checkpoint=False)),
    ]
    # Try both a conflicting and nonconflicting truncate.
    trunctime_values = [
        ('conflicting', dict(trunctime=15)),
        ('nonconflicting', dict(trunctime=25)),
    ]

    scenarios = make_scenarios(trunc_values,
        format_values, munge_values, eviction_values, checkpoint_values, trunctime_values)

    def evict(self, uri, key, value):
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        v = evict_cursor[key]
        self.assertEqual(v, value)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()

    def truncate(self, uri, make_key, keynum1, keynum2):
        if self.trunc_with_remove:
            cursor = self.session.open_cursor(uri)
            err = 0
            for k in range(keynum1, keynum2 + 1):
                cursor.set_key(k)
                try:
                    # In this test some or all the data is already deleted; skip those rows.
                    err = cursor.search()
                    if err == wiredtiger.WT_NOTFOUND:
                        err = 0
                    else:
                        err = cursor.remove()
                except wiredtiger.WiredTigerError as e:
                    if wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK) in str(e):
                        err = wiredtiger.WT_ROLLBACK
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
            except wiredtiger.WiredTigerError as e:
                if wiredtiger.wiredtiger_strerror(wiredtiger.WT_ROLLBACK) in str(e):
                    err = wiredtiger.WT_ROLLBACK
                else:
                    raise e
            lo_cursor.close()
            hi_cursor.close()
        return err

    def test_truncate06(self):
        nrows = 10000

        table_uri = 'table:truncate06'
        ds = SimpleDataSet(
            self, table_uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()
        self.session.checkpoint()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
        else:
            value_a = 'a' * 500
            value_b = 'b' * 500

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write a bunch of data at time 10.
        cursor = self.session.open_cursor(ds.uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value_a
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # This data can be stable; move the stable timestamp forward.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Munge some of it at time 20. Touch every other even-numbered key in the middle third of
        # the data. (This allows using the odd keys to evict.)
        self.session.begin_transaction()
        start = nrows // 3
        if start % 2 == 1:
            start += 1
        for i in range(start, 2 * nrows // 3, 2):
            cursor.set_key(ds.key(i))
            if self.munge_with_update:
                cursor.set_value(value_b)
                self.assertEqual(cursor.update(), 0)
            else:
                self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.reset()

        # Evict the lot so that we can fast-truncate.
        # For now, evict every 4th key explicitly; FUTURE: improve this to evict each page only
        # once when we have a good way to do that. 
        if self.do_evict:
            for i in range(1, nrows + 1, 4):
                self.evict(ds.uri, ds.key(i), value_a)

        if self.do_checkpoint:
            self.session.checkpoint()

        # Truncate at either time 15 or 25. Time 15 is across the deletions that are at 20
        # and should fail with WT_ROLLBACK; time 25 should succeed.
        ts = self.trunctime
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts - 1))
        err = self.truncate(ds.uri, ds.key, nrows // 4, nrows - nrows // 4)
        if ts == 15:
            self.assertEqual(err, wiredtiger.WT_ROLLBACK)
            self.session.rollback_transaction()
        else:
            self.assertEqual(err, 0)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))

        # Move the stable timestamp forward before exiting so we don't waste time rolling
        # back the rest of the changes during shutdown.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        
if __name__ == '__main__':
    wttest.run()
