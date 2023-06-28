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
from rollback_to_stable_util import test_rollback_to_stable_base
from wiredtiger import stat, WT_NOTFOUND
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_truncate14.py
# Generate very large namespace gaps with truncate.

class test_truncate14(wttest.WiredTigerTestCase):
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
        # Do not run this test on FLCS. It would need to materialize the entire key space,
        # which would be extremely slow and use extremely large amounts of memory.
        #('column_fix', dict(key_format='r', value_format='8t',
        #    extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('integer_row', dict(key_format='i', value_format='S', extraconfig='')),
    ]
    action_values = [
        ('instantiate', dict(action='instantiate')),
        ('checkpoint', dict(action='checkpoint')),
        ('checkpoint-visible', dict(action='checkpoint-visible')),
    ]
    scenarios = make_scenarios(trunc_values, format_values, action_values)

    # Make all the values different to avoid having VLCS RLE condense the table.
    def mkdata(self, basevalue, i):
        if self.value_format == '8t':
            return basevalue
        return basevalue + str(i)

    def truncate(self, uri, key1, key2):
        if self.trunc_with_remove:
            # Because remove clears the cursor position, removing by cursor-next is a nuisance.
            scan_cursor = self.session.open_cursor(uri)
            del_cursor = self.session.open_cursor(uri)
            err = 0
            scan_cursor.set_key(key1)
            self.assertEqual(scan_cursor.search(), 0)
            while scan_cursor.get_key() <= key2:
                del_cursor.set_key(scan_cursor.get_key())
                try:
                    err = del_cursor.remove()
                except WiredTigerError as e:
                    if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                        err = WT_ROLLBACK
                    elif wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                        err = WT_PREPARE_CONFLICT
                    else:
                        raise e
                if err != 0:
                    break
                scan_cursor.next()
            scan_cursor.close()
            del_cursor.close()
        else:
            lo_cursor = self.session.open_cursor(uri)
            hi_cursor = self.session.open_cursor(uri)
            lo_cursor.set_key(key1)
            hi_cursor.set_key(key2)
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
        self.assertEqual(err, 0)

    def check(self, uri, nrows, read_ts, basevalue):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = self.session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            value = self.mkdata(basevalue, k)
            self.assertEqual(v, value)
            count += 1
        self.session.commit_transaction()
        self.assertEqual(count, nrows)
        cursor.close()

    def test_truncate(self):
        blob_rows = 1000
        sparse_rows = 20000
        sparse_gap = 1000000000

        # Create a table without logging.
        uri = "table:truncate14"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)' + self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
        else:
            value_a = "aaaaa" * 100

        # Pin oldest and stable timestamps to 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        cursor = self.session.open_cursor(uri)

        # Write out some data at time 20. This won't fit in a single transaction,
        # but we'll treat it all as a single logical unit with the same commit
        # timestamp.
        self.session.begin_transaction()

        # Write out a dense blob of data at the beginning of the namespace.
        k = 1
        n = 0
        for i in range(1, blob_rows + 1):
            cursor[k] = self.mkdata(value_a, k)
            k += 1
            n += 1
            # Commit periodically to avoid overflowing the cache.
            if n % 1009 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
                self.session.begin_transaction()

        # We'll start deleting from the first sparse key.
        first = k

        # Write out a bunch of very sparse data.
        # (Intentionally don't commit at this specific point. If something goes wrong on a
        # per-transaction basis, it'll be easier to figure out what and how if that doesn't
        # intersect the test's logical groupings.)
        for i in range (1, sparse_rows + 1):
            cursor[k] = self.mkdata(value_a, k)
            k += sparse_gap
            n += 1
            # Commit periodically to avoid overflowing the cache.
            if n % 1009 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
                self.session.begin_transaction()

        # We'll delete up to (and including) the first key in the second blob.
        last = k

        # Now write another blob.
        for i in range(1, blob_rows + 1):
            cursor[k] = self.mkdata(value_a, k)
            k += 1
            n += 1
            # Commit periodically to avoid overflowing the cache.
            if n % 1009 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
                self.session.begin_transaction()

        # Commit this at time 20.
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.close()

        # Read back the data, just in case.
        self.check(uri, blob_rows + sparse_rows + blob_rows, 25, value_a)

        # Mark it stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Reopen the connection to flush the cache so we can fast-truncate.
        self.reopen_conn()

        # Truncate the entire sparse range at time 30.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        self.truncate(uri, first, last)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Check stats to make sure we fast-deleted at least one page.
        # (Except if we're running with trunc_with_remove.)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        if self.trunc_with_remove:
            self.assertEqual(fastdelete_pages, 0)
        else:
            self.assertGreater(fastdelete_pages, 0)

        # Mark it stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        # Read back the (non-truncated) data.
        self.check(uri, blob_rows + blob_rows - 1, 35, value_a)

        # Since this test is specifically about large namespace gaps, we don't need to
        # exercise every combination of truncate states; just the ones where the size of
        # the gap might be relevant. That means:
        #    1. The instantiate code, because it loops over keys.
        #    2. Internal page reconciliation when the truncate is not yet globally visible.
        #    3. Internal page reconciliation when the truncate is globally visible.
        # (It would be nice to test both checkpointing and eviction of internal pages, but
        # we can't force internal page evictions from Python.)

        if self.action == "instantiate":
            # Optionally read back the data before the truncate.
            self.check(uri, blob_rows + sparse_rows + blob_rows, 25, value_a)
        elif self.action == "checkpoint":
            self.session.checkpoint()
        else:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(30))
            self.session.checkpoint()

        # Read back the data again.
        self.check(uri, blob_rows + blob_rows - 1, 35, value_a)

if __name__ == '__main__':
    wttest.run()
