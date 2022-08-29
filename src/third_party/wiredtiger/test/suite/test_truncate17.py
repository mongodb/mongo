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

# test_truncate17.py
#
# Make sure that no shenanigans occur if we try to read from a page that's been
# fast-truncated by a prepared transaction.

class test_truncate17(wttest.WiredTigerTestCase):
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
    checkpoint_values = [
        ('no_checkpoint', dict(do_checkpoint=False)),
        ('checkpoint', dict(do_checkpoint=True)),
    ]
    scenarios = make_scenarios(trunc_values, format_values, checkpoint_values)

    def stat_tree(self, uri):
        statscursor = self.session.open_cursor('statistics:' + uri, None, 'statistics=(all)')

        entries = statscursor[stat.dsrc.btree_entries][2]
        if self.value_format == '8t':
            leaf_pages = statscursor[stat.dsrc.btree_column_fix][2]
            internal_pages = statscursor[stat.dsrc.btree_column_internal][2]
        elif self.key_format == 'r':
            leaf_pages = statscursor[stat.dsrc.btree_column_variable][2]
            internal_pages = statscursor[stat.dsrc.btree_column_internal][2]
        else:
            leaf_pages = statscursor[stat.dsrc.btree_row_leaf][2]
            internal_pages = statscursor[stat.dsrc.btree_row_internal][2]

        return (entries, (leaf_pages, internal_pages))

    def truncate(self, session, uri, make_key, keynum1, keynum2):
        if self.trunc_with_remove:
            cursor = session.open_cursor(uri)
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
            lo_cursor = session.open_cursor(uri)
            hi_cursor = session.open_cursor(uri)
            lo_cursor.set_key(make_key(keynum1))
            hi_cursor.set_key(make_key(keynum2))
            try:
                err = session.truncate(None, lo_cursor, hi_cursor, None)
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                    err = WT_ROLLBACK
                else:
                    raise e
            lo_cursor.close()
            hi_cursor.close()
        return err

    def test_truncate17(self):
        nrows = 10000

        # Create a table.
        uri = "table:truncate17"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
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
        cursor = self.session.open_cursor(ds.uri)
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

        # Reopen the connection so as to stat the on-disk version of the tree.
        self.reopen_conn()

        # Stat the tree to get a baseline.
        (base_entries, base_pages) = self.stat_tree(uri)
        self.assertEqual(base_entries, nrows)

        # Reopen the connection again so nothing is in memory and we can fast-truncate.
        self.reopen_conn()

        # Make a session to prepare in.
        session2 = self.conn.open_session()

        # Truncate the middle of the table.
        # 
        # Prepare the truncate at time 20 and leave it hanging.
        session2.begin_transaction()
        err = self.truncate(session2, ds.uri, ds.key, nrows // 4 + 1, 3 * nrows // 4)
        self.assertEqual(err, 0)
        session2.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        # Make sure we did at least one fast-delete. (Unless we specifically didn't want to,
        # or running on FLCS where it isn't supported.)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        if self.value_format == '8t' or self.trunc_with_remove:
            self.assertEqual(fastdelete_pages, 0)
        else:
            self.assertGreater(fastdelete_pages, 0)
        stat_cursor.close()

        # Optionally checkpoint at this stage, just in case it breaks or trips on
        # the prepared truncation.
        if self.do_checkpoint:
            self.session.checkpoint()

        # Stat the tree again. Stats are not transactional, and are effectively
        # read-uncommitted; we should see the results of the prepared truncate.
        # However, the truncated pages aren't actually gone yet, so the page counts
        # shouldn't change.
        (entries, pages) = self.stat_tree(uri)
        if self.value_format == '8t':
            self.assertEqual(entries, nrows)
        else:
            self.assertEqual(entries, nrows // 2)
        self.assertEqual(pages, base_pages)

        # This should instantiate all the deleted pages.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        read_deleted = stat_cursor[stat.conn.cache_read_deleted][2]
        self.assertEqual(read_deleted, fastdelete_pages)
        stat_cursor.close()

        # Now toss the prepared transaction.
        session2.rollback_transaction()

        # Unlike RTS, transaction rollback should not instantiate pages, plus there are
        # no more deleted pages to instantiate, so the number of instantiated pages should
        # remain unchanged.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        read_deleted = stat_cursor[stat.conn.cache_read_deleted][2]
        self.assertEqual(read_deleted, fastdelete_pages)
        stat_cursor.close()

if __name__ == '__main__':
    wttest.run()
