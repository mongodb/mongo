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

# test_checkpoint14.py
#
# Make sure each checkpoint has its own snapshot by creating two successive
# inconsistent checkpoints and reading both of them.

class test_checkpoint(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all),timing_stress_for_test=[checkpoint_slow]'
    session_config = 'isolation=snapshot'

    format_values = [
        ('column-fix', dict(key_format='r', value_format='8t',
            extraconfig=',allocation_size=512,leaf_page_max=512')),
        ('column', dict(key_format='r', value_format='S', extraconfig='')),
        ('string_row', dict(key_format='S', value_format='S', extraconfig='')),
    ]
    name_values = [
        ('nn', dict(first_checkpoint='first_checkpoint', second_checkpoint='second_checkpoint')),
        ('nu', dict(first_checkpoint='first_checkpoint', second_checkpoint=None)),
        # This doesn't work because there's no way to open the first unnamed checkpoint.
        #('un', dict(first_checkpoint=None, second_checkpoint='second_checkpoint')),
    ]
    scenarios = make_scenarios(format_values, name_values)

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
    def check(self, ds, ckpt, nrows, value):
        if ckpt is None:
            ckpt = 'WiredTigerCheckpoint'
        cursor = self.session.open_cursor(ds.uri, None, 'checkpoint=' + ckpt)
        #self.session.begin_transaction()
        count = 0
        for k, v in cursor:
            self.assertEqual(v, value)
            count += 1
        self.assertEqual(count, nrows)
        #self.session.rollback_transaction()
        cursor.close()

    def test_checkpoint(self):
        uri = 'table:checkpoint14'
        nrows = 10000

        # Create a table.
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config=self.extraconfig)
        ds.populate()

        if self.value_format == '8t':
            nrows *= 5
            value_a = 97
            value_b = 98
            value_c = 99
        else:
            value_a = "aaaaa" * 100
            value_b = "bbbbb" * 100
            value_c = "ccccc" * 100

        # Write some baseline data.
        self.large_updates(uri, ds, nrows, value_a)
        # Write this data out now so we aren't waiting for it while trying to
        # race with the later data.
        self.session.checkpoint()

        # Write some more data, and hold the transaction open.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        for i in range(1, nrows + 1):
            cursor2[ds.key(i)] = value_b

        # Checkpoint in the background.
        done = threading.Event()
        if self.first_checkpoint is None:
            ckpt = checkpoint_thread(self.conn, done)
        else:
            ckpt = named_checkpoint_thread(self.conn, done, self.first_checkpoint)
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

        # Rinse and repeat.
        session2.begin_transaction()
        for i in range(1, nrows + 1):
            cursor2[ds.key(i)] = value_c

        # Checkpoint in the background.
        done = threading.Event()
        if self.second_checkpoint is None:
            ckpt = checkpoint_thread(self.conn, done)
        else:
            ckpt = named_checkpoint_thread(self.conn, done, self.second_checkpoint)
        try:
            ckpt.start()
            # Sleep a bit so that checkpoint starts before committing last transaction.
            time.sleep(2)
            session2.commit_transaction()
        finally:
            done.set()
            ckpt.join()

        # Other tests check for whether the visibility of a partially-written transaction
        # is handled correctly. Here we're interested in whether the visibility mechanism
        # is using the right snapshot for the checkpoint we're reading. So insist that we
        # not see the value_b transaction in the first checkpoint, or the value_c transaction
        # in the second checkpoint. If test machine lag causes either transaction to commit
        # before the checkpoint starts, we'll see value_b in the first checkpoint and/or
        # value_c in the second. But also, if we end up using the second checkpoint's snapshot
        # for the first checkpoint, we'll see value_b. So if this happens more than once in a
        # blue moon we should probably strengthen the test so we can more reliably distinguish
        # the cases, probably by doing a third transaction/checkpoint pair.
        #
        # If we end up using the first checkpoint's snapshot for reading the second checkpoint,
        # we'll most likely see no data at all; that would be a serious failure if it happened.

        # Read the checkpoints.
        self.check(ds, self.first_checkpoint, nrows, value_a)
        self.check(ds, self.second_checkpoint, nrows, value_b)

        # If we haven't died yet, pretend to crash, and run RTS to see if the
        # (second) checkpoint was inconsistent. Unfortunately we can't readily
        # check on both.
        simulate_crash_restart(self, ".", "RESTART")

        # Make sure we did get an inconsistent checkpoint.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        inconsistent_ckpt = stat_cursor[stat.conn.txn_rts_inconsistent_ckpt][2]
        stat_cursor.close()
        self.assertGreater(inconsistent_ckpt, 0)


if __name__ == '__main__':
    wttest.run()
