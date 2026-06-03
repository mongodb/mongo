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

# test_layered_schema08.py
#   Test that shared metadata queue operations are deferred until a checkpoint runs.
#
#   Schema operations (create, drop) on a leader enqueue metadata updates with deferred=true.
#   The checkpoint prepare step undefers all existing entries, and they are applied at the end
#   of that same checkpoint. Operations enqueued concurrently with a running checkpoint (after
#   prepare) remain deferred until the next checkpoint.

import threading, time, wiredtiger, wttest
from checkpoint_util import checkpoint_util
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema08(checkpoint_util):
    uri_base = 'test_layered_schema08'
    conn_config = 'statistics=(all),disaggregated=(role="leader",lose_all_my_data=true)'

    table_types = [
        ('layered-prefix', dict(prefix='layered:', table_config='')),
        ('table-prefix', dict(prefix='table:', table_config=',block_manager=disagg,type=layered')),
    ]

    disagg_storages = gen_disagg_storages('test_layered_schema08', disagg_only=True)
    scenarios = make_scenarios(table_types, disagg_storages)

    def uri(self):
        return self.prefix + self.uri_base

    def stable_uri(self):
        return 'file:' + self.uri_base + '.wt_stable'

    def layered_uri(self):
        return 'layered:' + self.uri_base

    def shared_meta_uris(self):
        uris = [self.stable_uri(), self.layered_uri()]
        if self.prefix == 'table:':
            uris += ['table:' + self.uri_base, 'colgroup:' + self.uri_base]
        return uris

    def check_shared_metadata(self, expect_contains=None, expect_missing=None):
        cursor = self.session.open_cursor('file:WiredTigerShared.wt_stable', None, None)
        metadata = {}
        while cursor.next() == 0:
            metadata[cursor.get_key()] = cursor.get_value()
        cursor.close()
        for uri in (expect_contains or []):
            self.assertIn(uri, metadata)
        for uri in (expect_missing or []):
            self.assertNotIn(uri, metadata)

    def test_create_deferred_until_checkpoint(self):
        """
        A CREATE operation is enqueued with deferred=true and must not appear in the
        shared metadata table until a checkpoint runs.
        """
        self.session.create(self.uri(), 'key_format=S,value_format=S' + self.table_config)

        # The create metadata operation is in the queue but still deferred: the shared
        # metadata table must not reflect it yet.
        self.check_shared_metadata(expect_missing=self.shared_meta_uris())

        # After a checkpoint the entry is undeferred (at prepare time) and applied (at
        # the end of the checkpoint).
        self.session.checkpoint()
        self.check_shared_metadata(expect_contains=self.shared_meta_uris())

    def test_drop_deferred_until_checkpoint(self):
        """
        A REMOVE operation is enqueued with deferred=true. The table must still appear
        in the shared metadata table after the drop, and disappear only after the next
        checkpoint.
        """
        self.session.create(self.uri(), 'key_format=S,value_format=S' + self.table_config)
        self.session.checkpoint()
        self.check_shared_metadata(expect_contains=self.shared_meta_uris())

        self.session.drop(self.uri(), '')

        # The REMOVE entry is in the queue but still deferred: the shared metadata table
        # must still contain the table until a checkpoint processes the queue.
        self.check_shared_metadata(expect_contains=self.shared_meta_uris())

        # After a checkpoint the remove is applied.
        self.session.checkpoint()
        self.check_shared_metadata(expect_missing=self.shared_meta_uris())

    def test_create_drop_same_checkpoint(self):
        """
        Create and drop a table, then run a single checkpoint. Both the CREATE and REMOVE
        entries are undeferred at checkpoint prepare time and both are applied at checkpoint
        end, so the table must not be in the shared metadata table afterwards.
        """
        self.session.create(self.uri(), 'key_format=S,value_format=S' + self.table_config)
        self.session.drop(self.uri(), '')

        self.session.checkpoint()
        self.check_shared_metadata(expect_missing=self.shared_meta_uris())

    def test_multiple_checkpoints(self):
        """
        Verify that entries survive across multiple checkpoints as expected: a table
        created in checkpoint N and dropped in checkpoint N+1 is present in shared
        metadata between those two checkpoints.
        """
        self.session.create(self.uri(), 'key_format=S,value_format=S' + self.table_config)
        self.session.checkpoint()
        self.check_shared_metadata(expect_contains=self.shared_meta_uris())

        # An additional checkpoint without any schema changes must leave shared metadata
        # unchanged.
        self.session.checkpoint()
        self.check_shared_metadata(expect_contains=self.shared_meta_uris())

        self.session.drop(self.uri(), '')
        self.session.checkpoint()
        self.check_shared_metadata(expect_missing=self.shared_meta_uris())

    def test_create_during_checkpoint_deferred_to_next(self):
        """
        A table created while a checkpoint is already running must be deferred to the
        next checkpoint. checkpoint_prepare releases the schema lock before the bulk of
        the checkpoint work happens, so a concurrent create enqueues its entry with
        deferred=true after prepare has run. That entry is skipped by queue_process at
        the end of the current checkpoint (clearing the flag) and applied only at the
        end of the following checkpoint.
        """
        # Slow the checkpoint to >=10 seconds so we have a reliable window to create a
        # table after checkpoint_prepare has run but before the checkpoint completes.
        self.conn.reconfigure('timing_stress_for_test=[checkpoint_slow]')

        def run_checkpoint(conn):
            session = conn.open_session('')
            session.checkpoint()
            session.close()

        ckpt_thread = threading.Thread(target=run_checkpoint, args=(self.conn,))
        ckpt_thread.start()

        # checkpoint_state becomes non-zero only after checkpoint_prepare has completed
        # and the schema lock has been released, so it is safe to create the table now.
        self.wait_for_checkpoint_start()
        time.sleep(0.1)

        self.session.create(self.uri(), 'key_format=S,value_format=S' + self.table_config)
        ckpt_thread.join()

        self.conn.reconfigure('timing_stress_for_test=[]')

        # The first checkpoint completed without applying the create (the entry was
        # deferred): the table must not yet be in the shared metadata table.
        self.check_shared_metadata(expect_missing=self.shared_meta_uris())

        # The second checkpoint clears the deferred flag at prepare time and applies it at
        # the end: the table must now appear in the shared metadata table.
        self.session.checkpoint()
        self.check_shared_metadata(expect_contains=self.shared_meta_uris())

    def test_drop_during_checkpoint_deferred_to_next(self):
        """
        A table dropped while a checkpoint is already running must be deferred to the
        next checkpoint. The REMOVE entry is enqueued with deferred=true after prepare
        has released the schema lock. queue_process skips it in the current checkpoint
        (clearing the flag), and applies it only at the end of the following checkpoint.

        Dropping during a running checkpoint requires checkpoint_wait=false because the
        checkpoint holds the dhandle open. To ensure the dhandle is otherwise free, the
        table is created via a separate session that is then closed, and we wait for the
        sweep thread to close the idle handle before starting the concurrent test.
        """
        # Enable fast idle-handle sweep so the dhandle is released promptly after the
        # creating session closes, leaving the handle free for a concurrent drop.
        self.conn.reconfigure(
            'file_manager=(close_scan_interval=1,close_idle_time=1,close_handle_minimum=0)')

        # Snapshot the sweep counter before closing the session so that we detect the
        # increment that results specifically from the session2 close, not a stale one.
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        sweep_baseline = stat_cursor[wiredtiger.stat.conn.dh_sweep_dead_close][2]
        stat_cursor.close()

        # Create the table in a separate session so that closing it drops the only
        # non-sweep reference to the dhandle.
        session2 = self.conn.open_session('')
        session2.create(self.uri(), 'key_format=S,value_format=S' + self.table_config)
        session2.close()

        # Wait for the sweep to close the idle dhandle.
        while True:
            stat_cursor = self.session.open_cursor('statistics:', None, None)
            sweep_closes = stat_cursor[wiredtiger.stat.conn.dh_sweep_dead_close][2]
            stat_cursor.close()
            if sweep_closes > sweep_baseline:
                break
            time.sleep(0.5)

        # Checkpoint to get the table into the shared metadata table.
        self.session.checkpoint()
        self.check_shared_metadata(expect_contains=self.shared_meta_uris())

        # Slow the checkpoint to >=10 seconds so we have a reliable window to drop the
        # table after checkpoint_prepare has run but before the checkpoint completes.
        self.conn.reconfigure('timing_stress_for_test=[checkpoint_slow]')

        def run_checkpoint(conn):
            session = conn.open_session('')
            session.checkpoint()
            session.close()

        ckpt_thread = threading.Thread(target=run_checkpoint, args=(self.conn,))
        ckpt_thread.start()

        self.wait_for_checkpoint_start()
        time.sleep(0.5)

        # Drop while the checkpoint is running. checkpoint_wait=false lets the drop
        # proceed without waiting for the checkpoint to release the dhandle.
        self.session.drop(self.uri(), 'checkpoint_wait=false')
        ckpt_thread.join()

        self.conn.reconfigure('timing_stress_for_test=[]')

        # The REMOVE entry was deferred through the concurrent checkpoint: the table
        # must still be in the shared metadata table.
        self.check_shared_metadata(expect_contains=self.shared_meta_uris())

        # The next checkpoint clears the deferred flag and applies the removal: the
        # table must now be gone from the shared metadata table.
        self.session.checkpoint()
        self.check_shared_metadata(expect_missing=self.shared_meta_uris())
