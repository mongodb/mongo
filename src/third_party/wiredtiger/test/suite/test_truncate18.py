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

# test_truncate18.py
#
# The optimization that replaces deleted pages full of obsolete values with physically
# empty pages can cause problems, because for some purposes the empty page is not
# equivalent.
#
# In particular, the key order checks in row-store verify depend on the keys being
# physically present, and loading an empty page defeats that. This is more or less
# harmless except in the case of the leftmost leaf page, whose keys are used to
# initialize the check.
#
# It is not entirely trivial to reach the failure state, because the page under the start
# point of a truncate is never fast-truncated and that in turn means the leftmost page of
# the tree is never fast-truncated. Consequently, to get a deleted leftmost leaf we must
# truncate a range the beginning of the tree and then cause at least the first page of the
# range to be discarded while keeping some of the rest of it.
#
# The only way I've thought of to do this is to truncate a range that spans more than one
# internal page. Then the first internal page of the range can be reconciled (required to
# discard the non-deleted leftmost page) without discarding the whole truncated range.
#
# Consequently we crank down internal_page_max to avoid needing an excessively large test.
#
# Then we set things up so that the truncation becomes globally visible and run verify.
# That currently asserts. The fix for this is likely to disable the optimization when in
# verify, so the only real purpose of this test is to prevent the behavior from regressing.
# It is therefore not full of scenarios but specific to this one problem.

class test_truncate18(wttest.WiredTigerTestCase):
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
        ('integer_row', dict(key_format='i', value_format='S', extraconfig='')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
    ]
    trunc_range_values = [
        ('front', dict(truncate_front=True)),
        ('back', dict(truncate_front=False)),
    ]
    scenarios = make_scenarios(trunc_values, format_values, trunc_range_values)

    # Truncate, from keynum1 to keynum2, inclusive.
    def truncate(self, uri, make_key, keynum1, keynum2, read_ts, commit_ts):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
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
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        return err

    def test_truncate18(self):
        # With the small internal pages, 10000 rows is enough. 5000 rows is not.
        nrows = 10000

        # Create a table.
        uri = "table:truncate18"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='internal_page_max=4096' + self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100

        # Pin oldest and stable timestamps to 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write some baseline data at time 10.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value_a
            if i % 487 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()

        # Mark it stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Reopen the connection again so nothing is in memory and we can fast-truncate.
        self.reopen_conn()

        # Truncate most of the tree at time 20. Including either the start or end of the tree.
        if self.truncate_front:
            start_key = 1
            end_key = 7 * nrows // 8
        else:
            start_key = nrows // 8
            end_key = nrows
        err = self.truncate(ds.uri, ds.key, start_key, end_key, 15, 20)
        self.assertEqual(err, 0)

        # Make sure we did at least one fast-delete. (Unless we specifically didn't want to,
        # or running on FLCS where it isn't supported.)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        if self.value_format == '8t' or self.trunc_with_remove:
            self.assertEqual(fastdelete_pages, 0)
        else:
            self.assertGreater(fastdelete_pages, 0)
        stat_cursor.close()

        # Mark all this stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # Reopen the connection again so everything is purely on disk.
        self.reopen_conn()

        # Age out the baseline data, so the pages we truncated contain entirely obsolete data.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(30))

        # Since we didn't fast-truncate the first page (one can't) we need to get it
        # discarded by forcing it to reconcile empty. This will also discard all the
        # fast-truncated pages that are children of the first internal page. For the
        # test to work we need to have more fast-truncated pages beyond that, but there
        # is no good way to crosscheck if we do.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor[ds.key(1)] = value_b
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(35))
        self.session.begin_transaction()
        cursor.set_key(ds.key(1))
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(40))
        cursor.close()

        # Mark this change stable (and age out the scratch value we wrote) and checkpoint it.
        # This will reconcile the first leaf page and the first internal page, and internal
        # pages above that, but leave the second internal page alone since we did nothing to
        # bring it into memory.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(40))
        self.session.checkpoint()
        
        # Reopen the connection yet again.
        self.reopen_conn()

        # Now verify the tree. In the problem scenario described above, this will assert.
        self.session.verify(ds.uri, None)

if __name__ == '__main__':
    wttest.run()
