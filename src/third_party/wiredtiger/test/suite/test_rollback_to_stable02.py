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
from wiredtiger import stat
from wtscenario import make_scenarios
from rollback_to_stable_util import test_rollback_to_stable_base

# test_rollback_to_stable02.py
# Test that rollback to stable brings back the history value to replace on-disk value.
class test_rollback_to_stable02(test_rollback_to_stable_base):

    # For FLCS, set the page size down. Otherwise for the in-memory scenarios we get enough
    # updates on the page that the in-memory page footprint exceeds the default maximum
    # in-memory size, and that in turn leads to pathological behavior where the page gets
    # force-evicted over and over again trying to resolve/condense the updates. But they
    # don't (for in-memory, they can't be moved to the history store) so this leads to a
    # semi-livelock state that makes the test some 20x slower than it needs to be.
    #
    # FUTURE: it would be better if the system adjusted on its own, but it's not critical
    # and this workload (with every entry on the page modified repeatedly) isn't much like
    # anything that happens in production.
    format_values = [
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('column_fix', dict(key_format='r', value_format='8t', extraconfig=',leaf_page_max=4096')),
        ('row_integer', dict(key_format='i', value_format='S', extraconfig='')),
    ]

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    dryrun_values = [
        ('no_dryrun', dict(dryrun=False)),
        ('dryrun', dict(dryrun=True))
    ]

    worker_thread_values = [
        ('0', dict(threads=0)),
        ('4', dict(threads=4)),
        ('8', dict(threads=8))
    ]

    scenarios = make_scenarios(format_values, in_memory_values, prepare_values, dryrun_values, worker_thread_values)

    def conn_config(self):
        config = 'cache_size=100MB,statistics=(all),verbose=(rts:5)'
        if self.in_memory:
            config += ',in_memory=true'
        return config

    def test_rollback_to_stable(self):
        nrows = 10000

        # Create a table.
        uri = "table:rollback_to_stable02"
        ds_config = self.extraconfig
        ds_config += ',log=(enabled=false)' if self.in_memory else ''
        ds = SimpleDataSet(self, uri, 0,
            key_format=self.key_format, value_format=self.value_format, config=ds_config)
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
            valuec = 99
            valued = 100
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100
            valuec = "ccccc" * 100
            valued = "ddddd" * 100

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        self.large_updates(uri, valuea, ds, nrows, self.prepare, 10)
        # Check that all updates are seen.
        self.check(valuea, uri, nrows, None, 11 if self.prepare else 10)

        self.large_updates(uri, valueb, ds, nrows, self.prepare, 20)
        # Check that the new updates are only seen after the update timestamp.
        self.check(valueb, uri, nrows, None, 21 if self.prepare else 20)

        self.large_updates(uri, valuec, ds, nrows, self.prepare, 30)
        # Check that the new updates are only seen after the update timestamp.
        self.check(valuec, uri, nrows, None, 31 if self.prepare else 30)

        self.large_updates(uri, valued, ds, nrows, self.prepare, 40)
        # Check that the new updates are only seen after the update timestamp.
        self.check(valued, uri, nrows, None, 41 if self.prepare else 40)

        # Pin stable to timestamp 30 if prepare otherwise 20.
        if self.prepare:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        else:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        # Checkpoint to ensure that all the data is flushed.
        self.session.breakpoint()
        if not self.in_memory:
            self.session.checkpoint()

        self.conn.rollback_to_stable('dryrun={}'.format('true' if self.dryrun else 'false') + ',threads=' + str(self.threads))
        # Check that the new updates are only seen after the update timestamp.
        self.session.breakpoint()

        if self.dryrun:
            self.check(valued, uri, nrows, None, 40)
        else:
            self.check(valueb, uri, nrows, None, 40)

        self.check(valueb, uri, nrows, None, 20)
        self.check(valuea, uri, nrows, None, 10)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        calls = stat_cursor[stat.conn.txn_rts][2]
        upd_aborted = (stat_cursor[stat.conn.txn_rts_upd_aborted][2] +
            stat_cursor[stat.conn.txn_rts_hs_removed][2])
        upd_aborted_dryrun = (stat_cursor[stat.conn.txn_rts_upd_aborted_dryrun][2] +
            stat_cursor[stat.conn.txn_rts_hs_removed_dryrun][2])
        keys_removed = stat_cursor[stat.conn.txn_rts_keys_removed][2]
        keys_restored = stat_cursor[stat.conn.txn_rts_keys_restored][2]
        pages_visited = stat_cursor[stat.conn.txn_rts_pages_visited][2]
        stat_cursor.close()

        self.assertEqual(calls, 1)
        self.assertEqual(keys_removed, 0)
        self.assertEqual(keys_restored, 0)
        self.assertGreater(pages_visited, 0)

        if self.dryrun:
            self.assertEqual(upd_aborted, 0)
            self.assertGreaterEqual(upd_aborted_dryrun, nrows * 2)
        else:
            self.assertGreaterEqual(upd_aborted, nrows * 2)
            self.assertEqual(upd_aborted_dryrun, 0)
