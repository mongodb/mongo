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

import threading
import wttest
from wiredtiger import stat
from wtthread import checkpoint_thread

# test_checkpoint39.py
#
# Under precise checkpoints eviction bounds itself with the snapshot the running checkpoint
# publishes. The checkpoint generation is bumped before that snapshot is published, so for an
# interval at the start of every checkpoint the published snapshot is still the previous
# checkpoint's, whose transaction ids can be far behind. Adopting it makes every update in the tree
# look invisible: reconciliation selects nothing and writes the on-disk cells forward instead.
#
# Check that eviction declines a snapshot an earlier checkpoint published, and that data written
# alongside those checkpoints survives.
class test_checkpoint39(wttest.WiredTigerTestCase):
    conn_config = (
        'cache_size=5MB,'
        'precise_checkpoint=true,statistics=(all),'
        'timing_stress_for_test=[prepare_checkpoint_delay]'
    )
    uri = 'table:checkpoint39'

    # Keep writes, checkpoints and eviction overlapping until enough declines have accumulated.
    # A single decline is within the noise of how many trees eviction happens to visit in a window;
    # the batch cap bounds the runtime when the target is not reached.
    nrows = 200
    nbatches = 1500
    declines_wanted = 10
    value = 'v' * 400

    def get_stat(self, stat_name):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        value = stat_cursor[stat_name][2]
        stat_cursor.close()
        return value

    def test_checkpoint_eviction_snapshot_generation(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        ts = 1
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(ts) +
                                ',stable_timestamp=' + self.timestamp_str(ts))

        # Checkpoint continuously in the background so that writes and eviction overlap both the
        # interval at the start of each checkpoint, which the delay above holds open, and the gaps
        # between checkpoints.
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)
        ckpt.start()

        batches_written = 0
        declined = 0
        try:
            for batch in range(1, self.nbatches + 1):
                batches_written = batch
                for i in range(self.nrows):
                    ts += 1
                    self.session.begin_transaction()
                    cursor[batch * self.nrows + i] = self.value
                    self.session.commit_transaction(
                        'commit_timestamp=' + self.timestamp_str(ts))

                # Move the stable and oldest timestamps up so the data just written can be evicted
                # and the history store does not grow without bound.
                self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(ts) +
                                        ',stable_timestamp=' + self.timestamp_str(ts))

                declined = self.get_stat(stat.conn.eviction_ckpt_snapshot_declined)
                if declined >= self.declines_wanted:
                    break
        finally:
            done.set()
            ckpt.join()

        # Eviction must have turned down snapshots that no running checkpoint published. Without
        # the generation stamp and the retire, eviction adopts those snapshots instead and this
        # count stays at zero however long the loop above runs.
        self.assertGreaterEqual(declined, self.declines_wanted,
            'eviction declined only {} snapshots over {} batches, so it is adopting snapshots '
            'that do not belong to the running checkpoint'.format(declined, batches_written))

        # Every value written above must still be readable. A reconciliation that could select
        # nothing would have written the on-disk cells forward in place of these updates.
        cursor.close()
        cursor = self.session.open_cursor(self.uri)
        for batch in range(1, batches_written + 1):
            for i in range(0, self.nrows, 37):
                self.assertEqual(cursor[batch * self.nrows + i], self.value)
        cursor.close()
