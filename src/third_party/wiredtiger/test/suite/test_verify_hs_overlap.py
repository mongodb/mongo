#!/usr/bin/env python3
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

import os
import shutil
import threading
import time
import wiredtiger
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from rollback_to_stable_util import test_rollback_to_stable_base
from wiredtiger import stat
from wtthread import checkpoint_thread

# Mixes a DS value with start/stop times [A, B) with a history store version valid [0, B).
#
# 1. Write to keys at timestamp A, then delete at B on the even keys. Evict while oldest < A, so the
#    DS now has timestamps [A, B).
# 2. Advance oldest to between A and B, and advance stable to B. The [A, B) pages are already clean,
#    so the checkpoint references them and does not re-reconcile. The DS keeps its start A.
# 3. Checkpoint with the history_store_checkpoint_delay timing stress option, at
#    checkpoint_timestamp = stable = B. It copies the [A, B) cell onto the checkpoint image, then it
#    sleeps before the history store phase.
# 4. During the delay, write a newer value over the even keys and evict the page. Because oldest >
#    A, the value's start timestamps are globally visible and the reconcile pinned timestamp is
#    min(oldest, checkpoint_timestamp=B) >= A, so HS reconciliation zeroes each value's start. It
#    then lands in the history store with start/stop times [0, B).
# 5. The same checkpoint then captures that [0, B) record while the DS image still holds [A, B).
#
# Now, the same version exists in both HS and DS, HS valid during [0, B) and DS valid during [A, B).
class test_verify_hs_overlap(test_rollback_to_stable_base):
    nrows = 20

    conn_config = "cache_size=50MB,statistics=(all),timing_stress_for_test=[history_store_checkpoint_delay]"

    def evict(self, uri):
        evict_cur = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            evict_cur.set_key(self.ds.key(i))
            evict_cur.search()
            if i % 5 == 0:
                evict_cur.reset()
        evict_cur.close()
        self.session.rollback_transaction()

    def write(self, uri, value, ts, start=1, step=1):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(start, self.nrows + 1, step):
            cursor[self.ds.key(i)] = value
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(ts))
        cursor.close()

    # The odd keys stay live to make the evict cursor behave how we want. (Nothing special about odd
    # keys, may as well be the first key on the page or whatever. This is just the easiest way to
    # make sure at least one page has the properties we want.)
    def delete_even(self, uri, ts):
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(2, self.nrows + 1, 2):
            cursor.set_key(self.ds.key(i))
            cursor.remove()
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(ts))
        cursor.close()

    # Build the DS + HS duplicate, leave self.conn/self.session pointed at a reopened backup.
    def build_skewed_dup(self, uri, value, value2, ts_A, ts_B, ts_C):
        self.ds = SimpleDataSet(self, uri, 0, config="leaf_page_max=4096,memory_page_max=4096")
        self.ds.populate()

        # Oldest stays below A for the whole test so V is always required by a reader and reconcile
        # must preserve it. stable starts below A.
        self.conn.set_timestamp("oldest_timestamp=" + self.timestamp_str(1)
            + ",stable_timestamp=" + self.timestamp_str(10))

        # Commit at A on every key, then evict.
        self.write(uri, value, ts_A)
        self.evict(uri)

        # Delete the even keys at B, then evict while stable < B so times [A, B) are written to
        # disk.
        self.delete_even(uri, ts_B)
        self.evict(uri)

        # Advance oldest into (A, B), and stable to B, before the checkpoint. The [A, B) leaf is
        # already clean + on disk, so the checkpoint references it and the cell keeps its start A.
        # Advancing oldest past A is what causes V's start to become globally visible and cleared to
        # 0. stable = B means the restart RTS keeps both the [A, B) data store cell and the [0, B)
        # history store record instead of rolling them back.
        self.conn.set_timestamp("oldest_timestamp=" + self.timestamp_str(ts_A + 1)
            + ",stable_timestamp=" + self.timestamp_str(ts_B))

        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done, checkpoint_count_max=1)
        try:
            ckpt.start()

            # Wait until the checkpoint has acquired its snapshot, then let it finish the data store
            # phase and start the delay.
            snapshot = 0
            while not snapshot:
                time.sleep(0.5)
                snapshot = self.get_stat(stat.conn.checkpoint_snapshot_acquired)
            time.sleep(2)

            # Dirty the leaf so the clean even-key [A, B) cells are re-reconciled with no new update
            # of their own. With oldest > A the reconcile pinned timestamp is min(oldest,
            # checkpoint=B) >= A, so the value's start is globally visible and gets zeroed.
            self.write(uri, value2, ts_C, start=1, step=2)
            self.evict(uri)

            # Supersede the even keys with a newer value. Reconcile reads the now-zeroed start 0
            # from the on-disk cell, pairs it with the stop at B, and moves the superseded version
            # into the history store as [0, B). The checkpoint then captures that record in its
            # delayed history store phase, so the same checkpoint has HS times [0, B) and DS times
            # [A, B).
            self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(ts_B + 5))
            self.write(uri, value2, ts_C, start=2, step=2)
            self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(ts_C + 5))
            self.evict(uri)
        finally:
            done.set()
            ckpt.join()

        # The duplicate exists only in the checkpoint the thread took: its data store image holds
        # [A, B) and its history store image holds A->B. A new checkpoint would re-reconcile the
        # now-stable delete and drop [A, B) from the data store, hiding the overlap.
        #
        # To solve this, copy the on-disk checkpoint with a backup cursor and reopen it so verify
        # can get exclusive access. The backup captures the checkpoint with the differing start
        # times but not the dirty live page. They're both below stable, so the restart RTS keeps
        # them.
        backup_dir = "BACKUP"
        os.makedirs(backup_dir, exist_ok=True)
        bkup_c = self.session.open_cursor("backup:", None, None)
        while bkup_c.next() == 0:
            shutil.copy(bkup_c.get_key(), backup_dir)
        bkup_c.close()

        self.close_conn()
        self.conn = self.setUpConnectionOpen(backup_dir)
        self.session = self.setUpSessionOpen(self.conn)


    def test_verify_hs_overlap(self):
        uri = "table:wt_verify_hs_overlap"
        self.build_skewed_dup(uri, "abcde" * 4, "fghij" * 4, 20, 30, 40)
        self.session.verify(uri, None)

    def test_rts_dup_consistency(self):
        # Sanity check that RTS handles the DS/HS duplicate. Having stable < B makes RTS rewrite
        # both copies.
        uri = "table:rts_dup_consistency"
        value = "abcde" * 4
        ts_A = 20
        ts_B = 30
        ts_C = 40
        self.build_skewed_dup(uri, value, "fghij" * 4, ts_A, ts_B, ts_C)

        self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(ts_B - 5) + ",force=true")
        self.conn.rollback_to_stable()

        # delete@B and V2@C are above stable, so they're rolled back. Version at A is live.
        self.check(value, uri, self.nrows, ts_A + 1)
        self.check(value, uri, self.nrows, ts_C + 10)
