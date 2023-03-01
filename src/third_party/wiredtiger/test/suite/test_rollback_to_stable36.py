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
from rollback_to_stable_util import verify_rts_logs
from wiredtiger import stat, WiredTigerError, wiredtiger_strerror, WT_ROLLBACK
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_rollback_to_stable36.py
#
# Check the behavior of a fast-truncated page where the truncation is not stable but
# everything else on the page is.

class test_rollback_to_stable36(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all),verbose=(rts:5)'
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
    rollback_modes = [
        ('runtime', dict(crash=False)),
        ('recovery', dict(crash=True)),
    ]
    scenarios = make_scenarios(trunc_values, format_values, rollback_modes)

    # Don't raise errors for these, the expectation is that the RTS verifier will
    # run on the test output.
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RTS')
        self.addTearDownAction(verify_rts_logs)

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

    def check(self, ds, value, nrows, ts):
        cursor = self.session.open_cursor(ds.uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        self.session.rollback_transaction()
        cursor.close()

    def test_rollback_to_stable36(self):
        nrows = 10000

        # Create a table.
        uri = "table:rollback_to_stable36"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            value_a = 97
        else:
            value_a = "aaaaa" * 100

        # Pin oldest and stable timestamps to 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Write some baseline data to table 1 at time 10.
        cursor1 = self.session.open_cursor(ds.uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor1[ds.key(i)] = value_a
            if i % 109 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor1.close()

        # Mark it stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Reopen the connection so nothing is in memory and we can fast-truncate.
        self.reopen_conn()

        # Truncate most of the table.
        # Commit the truncate at time 20.
        self.session.begin_transaction()
        err = self.truncate(ds.uri, ds.key, 50, nrows - 50)
        self.assertEqual(err, 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Make sure we did at least one fast-delete. (Unless we specifically didn't want to,
        # or running on FLCS where it isn't supported.)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        fastdelete_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        if self.value_format == '8t' or self.trunc_with_remove:
            self.assertEqual(fastdelete_pages, 0)
        else:
            self.assertGreater(fastdelete_pages, 0)
        stat_cursor.close()

        # Checkpoint.
        self.session.checkpoint()

        # Roll back, either via crashing or by explicit RTS.
        if self.crash:
            simulate_crash_restart(self, ".", "RESTART")
        else:
            self.conn.rollback_to_stable()

        # Currently rolling back a fast-truncate works by instantiating the pages and
        # rolling back the instantiated updates, so we should see some page instantiations.
        # (But not for FLCS.)
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        read_deleted = stat_cursor[stat.conn.cache_read_deleted][2]
        if self.value_format == '8t' or self.trunc_with_remove:
            self.assertEqual(read_deleted, 0)
        else:
            self.assertGreater(read_deleted, 0)
        stat_cursor.close()

        # Validate the data; we should see all of it, since the truncations weren't stable.
        self.check(ds, value_a, nrows, 15)
        self.check(ds, value_a, nrows, 25)

if __name__ == '__main__':
    wttest.run()
