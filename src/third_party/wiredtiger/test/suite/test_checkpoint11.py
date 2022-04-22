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

import threading, time
import wttest
from wtthread import checkpoint_thread, named_checkpoint_thread
from helper import simulate_crash_restart
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_checkpoint11.py
# Test what happens if we create an inconsistent checkpoint and then try to
# open it for read. This version uses timestamps.

class test_checkpoint(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all),timing_stress_for_test=[checkpoint_slow]'
    session_config = 'isolation=snapshot'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    overlap_values = [
        ('overlap', dict(do_overlap=True)),
        ('no-overlap', dict(do_overlap=False, long_only=True)),
    ]
    stable_ts_values = [
        ('5', dict(stable_ts=5)),
        ('15', dict(stable_ts=15, long_only=True)),
        ('25', dict(stable_ts=25)),
        # Cannot do 35: we need to commit at 30 after starting the checkpoint.
    ]
    advance_values = [
        ('no-advance', dict(do_advance=False)),
        ('advance', dict(do_advance=True)),
    ]
    name_values = [
        # Reopening and unnamed checkpoints will not work as intended because the reopen makes
        # a new checkpoint.
        ('named', dict(second_checkpoint='second_checkpoint', do_reopen=False)),
        ('named_reopen', dict(second_checkpoint='second_checkpoint', do_reopen=True)),
        ('unnamed', dict(second_checkpoint=None, do_reopen=False, long_only=True)),
    ]
    scenarios = make_scenarios(format_values,
        overlap_values, stable_ts_values, advance_values, name_values)

    def large_updates(self, uri, ds, nrows, value, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value
            if i % 101 == 0:
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
                self.session.begin_transaction()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()

    # "ts_expected" is a map from timestamps to lists of maps from values to counts of values.
    # That is, ts_expected[ts] is a list of maps from values to counts of values; the map of
    # values to counts that we see should be in that list.
    def check(self, ds, ckpt, ts_expected):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        ts_seen = {}
        for ts in ts_expected:
            cfg = 'checkpoint=' + ckpt
            if ts is not None:
                cfg += ',debug=(checkpoint_read_timestamp=' + self.timestamp_str(ts) + ')'
            cursor = self.session.open_cursor(ds.uri, None, cfg)
            #self.session.begin_transaction()
            seen = {}
            for k, v in cursor:
                if v in seen:
                    seen[v] += 1
                else:
                    seen[v] = 1
            #for v in seen:
            #    pv = v if self.value_format == '8t' else v[0]
            #    self.prout("at {} seen {}: {}".format(ts, pv, seen[v]))
            ts_seen[ts] = seen
            #self.session.rollback_transaction()
            cursor.close()
        # Check in a separate loop so that all the values have been examined before failing.
        for ts in ts_expected:
            expected = ts_expected[ts]
            seen = ts_seen[ts]
            self.assertTrue(seen in expected)

    def test_checkpoint(self):
        uri = 'table:checkpoint11'
        nrows = 10000
        overlap = 5000 if self.do_overlap else 0
        morerows = 10000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            morerows *= 5
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100

        # Pin oldest and stable timestamps to 5.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5) +
            ',stable_timestamp=' + self.timestamp_str(5))

        # Write some data at time 10.
        self.large_updates(uri, ds, nrows, value_a, 10)

        # Write some more data at time 20.
        self.large_updates(uri, ds, nrows, value_b, 20)

        # Write this data out now so we aren't waiting for it while trying to
        # race with the later data.
        self.session.checkpoint()

        # Write some further data, and hold the transaction open. Eventually commit at time 30.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        #for i in range(1, nrows + 1, 10):
        for i in range(nrows - overlap + 1, nrows + morerows + 1):
            cursor2[ds.key(i)] = value_c

        # Optionally move stable forward.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.stable_ts))

        # Checkpoint in the background.
        done = threading.Event()
        if self.second_checkpoint is None:
            ckpt = checkpoint_thread(self.conn, done)
        else:
            ckpt = named_checkpoint_thread(self.conn, done, self.second_checkpoint)
        try:
            ckpt.start()

            # Wait for checkpoint to start before committing.
            ckpt_started = 0
            while not ckpt_started:
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_started = stat_cursor[stat.conn.txn_checkpoint_running][2]
                stat_cursor.close()
                time.sleep(1)

            session2.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        finally:
            done.set()
            ckpt.join()

        # Reopen if desired to cycle the write generations.
        if self.do_reopen:
            self.reopen_conn()

        # There are two states we should be able to produce. In all cases we should
        # see all the original data (value_a at time 10, value_b at time 20). If
        # the value_c transaction appears in the checkpoint we should see all the
        # additional data as well (value_c at time 30); otherwise reading past time 20
        # should yield value_b.
        #
        # It is ok to see either state in the checkpoint (since the checkpoint could
        # reasonably include or not include the second txn) but not ok to see
        # an intermediate state, and in particular we must not see _part_ of the value_c
        # data.
        expected_5 = {}
        expected_15 = { value_a: nrows }
        expected_25 = { value_b: nrows }
        expected_35_a = { value_b: nrows }
        expected_35_b = { value_b: nrows - overlap, value_c: overlap + morerows }
        expected = {
            5: [expected_5],
            15: [expected_15],
            25: [expected_25],
            35: [expected_35_a, expected_35_b]
        }
        # When reading without an explicit timestamp, we should see the state as of
        # the stable timestamp when the checkpoint was taken.
        expected[None] = expected[self.stable_ts]

        # For FLCS, because the table expands under uncommitted data, we should
        # see zeros once the additional data's been written (that is, always strictly
        # before the checkpoint) if we don't see the actual values.
        expected_5_flcs = { 0: nrows + morerows }
        expected_15_flcs = { value_a: nrows, 0: morerows }
        expected_25_flcs = { value_b: nrows, 0: morerows }
        expected_35_flcs_a = { value_b: nrows, 0: morerows }
        expected_35_flcs_b = { value_b: nrows - overlap, value_c: overlap + morerows }
        expected_flcs = {
            5: [expected_5_flcs],
            15: [expected_15_flcs],
            25: [expected_25_flcs],
            35: [expected_35_flcs_a, expected_35_flcs_b]
        }
        expected_flcs[None] = expected_flcs[self.stable_ts]

        if self.do_advance:
            # Move oldest up in case that interferes with handling the checkpoint.
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        # Now read the checkpoint.
        self.check(ds, self.second_checkpoint, expected_flcs if self.value_format == '8t' else expected)

        # If we haven't died yet, pretend to crash and run RTS to see if the
        # checkpoint was inconsistent.
        # (This only works if we didn't reopen the connection, so don't bother if we did.)
        if not self.do_reopen:
            simulate_crash_restart(self, ".", "RESTART")

            # Make sure we did get an inconsistent checkpoint.
            stat_cursor = self.session.open_cursor('statistics:', None, None)
            inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
            stat_cursor.close()
            self.assertGreater(inconsistent_ckpt, 0)

if __name__ == '__main__':
    wttest.run()
