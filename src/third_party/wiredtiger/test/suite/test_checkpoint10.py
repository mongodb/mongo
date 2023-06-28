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

# test_checkpoint10.py
# Test what happens if we create an inconsistent checkpoint and then try to
# open it for read. No timestamps in this version.

class test_checkpoint(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    overlap_values = [
        ('no-overlap', dict(do_overlap=False)),
        ('overlap', dict(do_overlap=True)),
    ]
    name_values = [
        # Reopening and unnamed checkpoints will not work as intended because the reopen makes
        # a new checkpoint.
        ('named', dict(second_checkpoint='second_checkpoint', do_reopen=False)),
        ('named_reopen', dict(second_checkpoint='second_checkpoint', do_reopen=True)),
        ('unnamed', dict(second_checkpoint=None, do_reopen=False)),
    ]
    log_values = [
        ('nonlogged', dict(do_log=False)),
        ('logged', dict(do_log=True)),
    ]
    scenarios = make_scenarios(format_values, overlap_values, name_values, log_values)

    def conn_config(self):
        cfg = 'statistics=(all),timing_stress_for_test=[checkpoint_slow]'
        if self.do_log:
            cfg += ',log=(enabled=true)'
        return cfg 

    def large_updates(self, uri, ds, nrows, value):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            cursor[ds.key(i)] = value
            if i % 101 == 0:
                self.session.commit_transaction()
                self.session.begin_transaction()
        self.session.commit_transaction()
        cursor.close()

    # "expected" is a list of maps from values to counts of values.
    def check(self, ds, ckpt, expected):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        cursor = self.session.open_cursor(ds.uri, None, 'checkpoint=' + ckpt)
        #self.session.begin_transaction()
        seen = {}
        for k, v in cursor:
            if v in seen:
                seen[v] += 1
            else:
                seen[v] = 1
        #for v in seen:
        #    self.prout("seen {}: {}".format(v if self.value_format == '8t' else v[0], seen[v]))
        self.assertTrue(seen in expected)
        #self.session.rollback_transaction()
        cursor.close()

    def test_checkpoint(self):
        uri = 'table:checkpoint10'
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
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100

        # Write some data.
        self.large_updates(uri, ds, nrows, value_a)
        # Write this data out now so we aren't waiting for it while trying to
        # race with the later data.
        self.session.checkpoint()

        # Write some more data, and hold the transaction open.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        for i in range(nrows - overlap + 1, nrows + morerows + 1):
            cursor2[ds.key(i)] = value_b

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

            session2.commit_transaction()
        finally:
            done.set()
            ckpt.join()

        # Reopen if desired to cycle the write generations.
        if self.do_reopen:
            self.reopen_conn()

        # There are two states we should be able to produce: one with the original
        # data and one with the additional data.
        #
        # It is ok to see either in the checkpoint (since the checkpoint could
        # reasonably include or not include the second txn) but not ok to see
        # an intermediate state.
        expected_a = { value_a: nrows }
        expected_b = { value_a: nrows - overlap, value_b: overlap + morerows }
        expected = [expected_a, expected_b]

        # For FLCS, because the table expands under uncommitted data, we should
        # see zeros once the additional data's been written (that is, always strictly
        # before the checkpoint) if we don't see the actual values.
        expected_flcs_a = { value_a: nrows, 0: morerows }
        expected_flcs = [expected_flcs_a, expected_b]

        # Now read the checkpoint.
        self.check(ds, self.second_checkpoint, expected_flcs if self.value_format == '8t' else expected)

        # If we haven't died yet, pretend to crash and run RTS to see if the
        # checkpoint was inconsistent.
        # (This only works if we didn't reopen the connection, so don't bother if we did.)
        #
        # Disable this crosscheck until we have a more reliable way to generate inconsistent
        # checkpoints (checkpoints with a torn transaction) on demand. The current method
        # waits until the checkpoint has started to begin committing, but there's still a
        # race where the checkpoint thread starts another checkpoint after the commit is
        # finished. Consequently, occasional failures occur in the testbed, which are a waste
        # of everyone's time.
        #if not self.do_reopen:
        #    simulate_crash_restart(self, ".", "RESTART")
        #
        #    # Make sure we did get an inconsistent checkpoint.
        #    stat_cursor = self.session.open_cursor('statistics:', None, None)
        #    inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
        #    stat_cursor.close()
        #    self.assertGreater(inconsistent_ckpt, 0)

if __name__ == '__main__':
    wttest.run()
