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
from rollback_to_stable_util import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_truncate13.py
# Test reading in the gaps created by a fast-delete.

class test_truncate13(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'
    conn_config = 'cache_size=50MB,statistics=(all),log=(enabled=false)'

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
    range_values = [
        ('start', dict(where="start")),
        ('middle', dict(where="middle")),
        ('end', dict(where="end")),
    ]
    ts_values = [
        ('unstable', dict(advance_stable=False, advance_oldest=False)),
        ('stable', dict(advance_stable=True, advance_oldest=False)),
        ('visible', dict(advance_stable=True, advance_oldest=True)),
    ]
    add_values = [
        ('add', dict(add_data=True)),
        ('noadd', dict(add_data=False)),
    ]
    scenarios = make_scenarios(trunc_values, format_values, range_values, ts_values, add_values)

    # Make all the values different to avoid having VLCS RLE condense the table.
    def mkdata(self, basevalue, i):
        if self.value_format == '8t':
            return basevalue
        return basevalue + str(i)

    def evict(self, uri, lo, hi, basevalue):
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction()

        # Evict every 3rd key to make sure we get all the pages but not write them out
        # over and over again any more than necessary. FUTURE: improve this to evict
        # each page once when we get a suitable interface for that.
        for i in range(lo, hi, 3):
            evict_cursor.set_key(i)
            self.assertEquals(evict_cursor.search(), 0)
            self.assertEquals(evict_cursor.get_value(), self.mkdata(basevalue, i))
            evict_cursor.reset()
        self.session.rollback_transaction()
        evict_cursor.close()

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
                    elif wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                        err = WT_PREPARE_CONFLICT
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
                elif wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                    err = WT_PREPARE_CONFLICT
                else:
                    raise e
            lo_cursor.close()
            hi_cursor.close()
        return err

    def check(self, uri, first, skipped, nrows, read_ts, basevalue):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = self.session.open_cursor(uri)
        count = 0
        key = 1
        for k, v in cursor:
            if self.value_format == '8t':
                if key >= first and key < first + skipped:
                    value = 0
                else:
                    value = self.mkdata(basevalue, k)
            else:
                if key == first:
                    key += skipped
                value = self.mkdata(basevalue, k)
            self.assertEqual(k, key)
            self.assertEqual(v, value)
            count += 1
            key += 1
        self.session.commit_transaction()
        self.assertEqual(count, skipped + nrows if self.value_format == '8t' else nrows)
        cursor.close()

    def test_truncate(self):
        nrows = 10000

        # Create a table without logging.
        uri = "table:truncate13"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)' + self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
            valuec = 99
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100
            valuec = "ccccc" * 100

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
        self.evict(uri, 1, nrows + 1, valueb)

        # Move stable to 25 (after the baseline data).
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(25))

        # Checkpoint.
        self.session.checkpoint()

        # Now fast-delete half the table at time 35.
        self.session.begin_transaction()
        if self.where == 'start':
            firstkey = 1
            lastkey = nrows // 2
        elif self.where == "middle":
            firstkey = nrows // 4 + 1
            lastkey = 3 * nrows // 4
        else:
            firstkey = nrows // 2 + 1
            lastkey = nrows
        self.truncate(uri, ds.key, firstkey, lastkey)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(35))

        # Check stats to make sure we fast-deleted at least one page.
        # (Except for FLCS, where it's not supported and we should fast-delete zero pages.)
        # (Or if we are running with trunc_with_remove.)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        if self.value_format == '8t' or self.trunc_with_remove:
            self.assertEqual(fastdelete_pages, 0)
        else:
            self.assertGreater(fastdelete_pages, 0)

        if self.advance_stable or self.advance_oldest:
            # Optionally make the truncation stable.
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        if self.advance_oldest:
            # Optionally make the truncation globally visible.
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(40))
        
        # Checkpoint again with the deletion.
        self.session.checkpoint()

        if self.add_data:
            # Write more data at time 45.
            cursor = self.session.open_cursor(uri)
            self.session.begin_transaction()
            for i in range(1, nrows + 1):
                cursor[ds.key(i)] = self.mkdata(valuec, i)
                # Make a new transaction every 97 keys so the transactions don't get huge.
                if i % 97 == 0:
                    self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(45))
                    self.session.begin_transaction()
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(45))
            cursor.close()

        # Now (if we didn't advance oldest) read behind the deletion.
        # We should see all the baseline data, and all the second batch of data.
        if not self.advance_oldest:
            self.check(uri, 1, 0, nrows, 20, valuea)
            self.check(uri, 1, 0, nrows, 30, valueb)

        # Read after the deletion too; we should see half the second batch of data.
        self.check(uri, firstkey, nrows // 2, nrows // 2, 40, valueb)

        # If we wrote more data, read it back.
        if self.add_data:
            self.check(uri, 1, 0, nrows, 50, valuec)

if __name__ == '__main__':
    wttest.run()
