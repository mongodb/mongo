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
# software under patent law.
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
from helper import simulate_crash_restart
from rollback_to_stable_util import test_rollback_to_stable_base
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wtthread import checkpoint_thread


# test_rollback_to_stable47.py
#
# Test that reconciliation handles correctly the chain shape produced when an
# obsolete check has removed a globally visible tombstone that was previously
# installed by RTS on top of an existing on-disk value. The chain then has an
# older durable_ts than the (now-stale) on-disk image, which reconciliation
# must accept rather than treat as an invariant violation.
#
# Phases:
# 1. Keys 1..5 commit ts=10, keys 6..10 commit ts=30. Checkpoint, take backup.
# 2. Reopen backup with stable=20. RTS tombstones keys 6..10 at no_ts.
# 3. Prepared re-insert of value_c on keys 6..10 (durable=26).
#    Chain: [value_c durable=26] -> [tombstone no_ts].
# 4. Advance oldest=stable=30; commit value_d at ts=35.
#    Chain: [value_d durable=35] -> [value_c durable=26] -> [tombstone].
# 5. Concurrent checkpoint + forced eviction with checkpoint_slow timing stress
#    widens the race. The obsolete check removes the tombstone, leaving the
#    chain in an out-of-order state that reconciliation must accept.
class test_rollback_to_stable47(test_rollback_to_stable_base):
    format_values = [
        ("column", dict(key_format="r", value_format="S")),
        ("row_integer", dict(key_format="i", value_format="S")),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        config = "cache_size=50MB,statistics=(all),verbose=(rts:5)"
        return config

    def test_rollback_to_stable(self):
        uri = "table:rollback_to_stable47"
        backup_dir = "BACKUP"

        # Small row count so stable and unstable keys share the same leaf page.
        nrows_stable = 5  # keys 1..5:  committed at ts=10 (stable once stable=20)
        nrows_unstable = 5  # keys 6..10: committed at ts=30 (unstable at stable=20)
        nrows = nrows_stable + nrows_unstable

        value_a = "aaaaa" * 100  # stable value written for keys 1..5
        value_b = "bbbbb" * 100  # unstable value written for keys 6..10
        value_c = "ccccc" * 100  # first re-insert for keys 6..10 (becomes stable)
        value_d = "ddddd" * 100  # second re-insert for keys 6..10 (unstable)

        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format
        )
        ds.populate()

        # Set oldest=5, stable=5 so we can commit at ts=10 and ts=30 without violating
        # the rule that commit_timestamp >= stable_timestamp.  We advance stable to 20
        # after all writes are done, making ts=10 keys stable and ts=30 keys unstable.
        self.conn.set_timestamp(
            "oldest_timestamp="
            + self.timestamp_str(5)
            + ",stable_timestamp="
            + self.timestamp_str(5)
        )

        # --- Phase 1: write mixed stable/unstable data onto the same page ---

        # Keys 1..5 at ts=10 (commit_ts=10 >= stable=5, valid).
        # After stable advances to 20 below, ts=10 <= stable=20 -> STABLE.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows_stable + 1):
            cursor[ds.key(i)] = value_a
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10))
        cursor.close()

        # Keys 6..10 at ts=30 (commit_ts=30 >= stable=5, valid).
        # After stable advances to 20 below, ts=30 > stable=20 -> UNSTABLE.
        # No prior stable value for these keys; their stable state is "not exists".
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(nrows_stable + 1, nrows + 1):
            cursor[ds.key(i)] = value_b
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(30))
        cursor.close()

        # Advance stable to 20: ts=10 is now stable, ts=30 is now unstable.
        self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(20))

        # Evict all rows so the mixed-stability content lands on disk.
        evict_cur = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction("ignore_prepare=true")
        for i in range(1, nrows + 1):
            evict_cur.set_key(ds.key(i))
            evict_cur.search()
            if i % 5 == 0:
                evict_cur.reset()
        evict_cur.close()
        self.session.rollback_transaction()

        # Checkpoint to flush the on-disk image before backup.
        self.session.checkpoint()

        # Verify state before backup: 5 stable + 5 unstable records.
        self.check(value_a, uri, nrows_stable, 10)

        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(30))
        cursor = self.session.open_cursor(uri)
        for i in range(nrows_stable + 1, nrows + 1):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), value_b)
        self.session.rollback_transaction()
        cursor.close()

        # --- Phase 2: take a backup ---
        os.makedirs(backup_dir, exist_ok=True)
        bkup_c = self.session.open_cursor("backup:", None, None)
        while bkup_c.next() == 0:
            shutil.copy(bkup_c.get_key(), backup_dir)
        bkup_c.close()

        # Open from the backup directory. WiredTiger treats this as crash recovery and
        # runs RTS automatically using stable_timestamp=20 from the checkpoint metadata.
        self.close_conn()

        self.conn = self.setUpConnectionOpen(backup_dir)
        self.session = self.setUpSessionOpen(self.conn)

        # --- Phase 3: verify backup-restore RTS removed the unstable keys ---
        # Keys 1..5: value_a (stable ts=10 <= stable=20) -- must remain.
        # Keys 6..10: tombstoned by RTS (value_b ts=30 > stable=20) -- must be absent.

        self.check(value_a, uri, nrows_stable, 10)

        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(30))
        cursor = self.session.open_cursor(uri)
        for i in range(nrows_stable + 1, nrows + 1):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()
        cursor.close()

        # --- Phase 4: first re-insert (prepared, becomes stable after stable advances) ---
        # Commit value_c with prepare_ts=24, commit_ts=25, durable_ts=26.
        # Update chain: [value_c ts=25] -> [tombstone no_ts (from backup-restore RTS)].

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(nrows_stable + 1, nrows + 1):
            cursor[ds.key(i)] = value_c
        self.session.prepare_transaction("prepare_timestamp=" + self.timestamp_str(24))
        self.session.timestamp_transaction("commit_timestamp=" + self.timestamp_str(25))
        self.session.timestamp_transaction(
            "durable_timestamp=" + self.timestamp_str(26)
        )
        self.session.commit_transaction()
        cursor.close()

        # Verify value_c is visible at ts=25.
        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(25))
        cursor = self.session.open_cursor(uri)
        for i in range(nrows_stable + 1, nrows + 1):
            cursor.set_key(ds.key(i))
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), value_c)
        self.session.rollback_transaction()
        cursor.close()

        # --- Phase 5: advance timestamps, second re-insert, parallel eviction + checkpoint ---
        # Advance oldest=30, stable=30: value_c (durable_ts=26 <= stable=30) is now STABLE.
        # Insert value_d at ts=35 (commit_ts=35 >= stable=30, valid; 35 > stable=30 -> UNSTABLE).
        # Update chain: [value_d ts=35] -> [value_c ts=25 (stable)] -> [tombstone no_ts].
        self.conn.set_timestamp(
            "oldest_timestamp=" + self.timestamp_str(30)
            + ",stable_timestamp=" + self.timestamp_str(30)
        )

        # Wait for the eviction server to advance oldest_id so the obsolete check
        # can remove the no_ts tombstone (a checkpoint() here would do the same).
        time.sleep(1)

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(nrows_stable + 1, nrows + 1):
            cursor[ds.key(i)] = value_d
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(35))
        cursor.close()

        self.conn.reconfigure("timing_stress_for_test=[checkpoint_slow]")

        # Run eviction in parallel with a checkpoint thread.
        # When ckpt_stress=True, checkpoint_slow timing stress makes the checkpoint yield
        # frequently, creating a race window where eviction and reconciliation can interact
        # with the globally visible tombstone (from backup-restore RTS) that sits beneath
        # value_c.  This exercises whether reconciliation incorrectly collapses the chain
        # in a way that confuses the subsequent RTS pass.
        done = threading.Event()
        ckpt = checkpoint_thread(self.conn, done)

        try:
            ckpt.start()

            # Wait for checkpoint to start and acquire its snapshot before committing.
            ckpt_snapshot = 0
            while not ckpt_snapshot:
                time.sleep(1)
                stat_cursor = self.session.open_cursor('statistics:', None, None)
                ckpt_snapshot = stat_cursor[stat.conn.checkpoint_snapshot_acquired][2]
                stat_cursor.close()

            # Evict all rows while the checkpoint is in progress.
            evict_cur = self.session.open_cursor(uri, None, "debug=(release_evict)")
            self.session.begin_transaction("ignore_prepare=true")
            for i in range(1, nrows + 1):
                evict_cur.set_key(ds.key(i))
                evict_cur.search()
                if i % 5 == 0:
                    evict_cur.reset()
            evict_cur.close()
            self.session.rollback_transaction()
        finally:
            done.set()
            ckpt.join()
