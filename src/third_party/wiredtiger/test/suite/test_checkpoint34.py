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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat
from helper import simulate_crash_restart

# test_checkpoint34.py
#
# Test precise checkpoint with fast truncate
@wttest.skip_for_hook("tiered", "FIXME-WT-14937: this is crashing for disagg.")
class test_checkpoint34(wttest.WiredTigerTestCase):

    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]

    conn_config = "precise_checkpoint=true"

    scenarios = make_scenarios(format_values)

    def get_fast_truncated_pages(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        stat_cursor.close()
        return pages

    def test_checkpoint(self):
        uri = 'table:checkpoint34'
        nrows = 200000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        value_a =  "aaaaa" * 100

        ts = 2

        # Write some initial data.
        cursor = self.session.open_cursor(ds.uri, None, None)
        for i in range(1, nrows + 1):
            self.session.begin_transaction()
            cursor[ds.key(i)] = value_a
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
            self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts)}')
            ts += 1

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts)}')

        self.reopen_conn()

        # Do an unstable fast truncate.
        ts = ts + 1
        truncate_session = self.conn.open_session()
        truncate_session.begin_transaction()
        cursor_start = truncate_session.open_cursor(uri)
        cursor_start.set_key(ds.key(nrows // 2))
        truncate_session.truncate(None, cursor_start, None, None)
        truncate_session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        fast_truncates_pages = self.get_fast_truncated_pages()
        self.assertGreater(fast_truncates_pages, 0)
        cursor_start.close()

        # Checkpoint.
        self.session.checkpoint()

        # Crash and restart
        simulate_crash_restart(self, ".", "RESTART")

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        self.assertEqual(stat_cursor[stat.conn.txn_rts_upd_aborted][2], 0)
        stat_cursor.close()

        cursor = self.session.open_cursor(ds.uri, None, None)
        for i in range(1, nrows + 1):
            self.assertEqual(cursor[ds.key(i)], value_a)

if __name__ == '__main__':
    wttest.run()
