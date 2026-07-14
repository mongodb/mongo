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

# Error handling and validation for the publish API on followers.
#
# Covers publish precondition failures (zero epoch, epoch not newer than the stable schema
# epoch, missing epoch) and the fatal case of stable data in an unpublished table.

import os
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema13(wttest.WiredTigerTestCase, suite_subprocess, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = f'layered:{test_name}'

    table_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    #
    # Helper methods
    #

    def setup_leader_with_epoch(self):
        """Seed the shared storage with a checkpoint carrying schema epoch 10."""
        self.set_stable_epoch(10)
        self.leader_checkpoint(1)

    def swap_roles(self, conn_follower):
        """Demote self.conn to follower and promote conn_follower to leader."""
        self.conn.reconfigure('disaggregated=(role="follower")')
        conn_follower.reconfigure('disaggregated=(role="leader")')

    def checkpoint_and_advance(self, epoch, stable_ts, conn_leader):
        """
        Take a leader checkpoint at the given stable epoch and timestamp, then have self.conn
        (acting as a follower) pick up the resulting checkpoint.
        """
        self.set_stable_epoch(epoch, conn_leader)
        session = conn_leader.open_session('')
        self.leader_checkpoint(stable_ts, conn_leader, session)
        session.close()
        self.disagg_advance_checkpoint(self.conn, conn_leader)

    def open_inspection_follower(self, store_home):
        """Open a fresh follower on the store left by the dead subprocess and pick up its
        last complete checkpoint."""
        conn = self.wiredtiger_open(
            store_home, self.extensionsConfig() + ',create,' + self.conn_config_follower)
        self.ignoreStdoutPattern('WT_VERB_RTS|(wiredtiger_open:.*WT_VERB_METADATA)')
        self.disagg_advance_checkpoint(conn, conn)
        return conn

    #
    # Publish precondition tests
    #

    def test_follower_publish_zero_epoch(self):
        """Publishing a follower-created table with a zero schema epoch returns EINVAL."""
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: session_follow.publish(self.uri, 'disaggregated=(schema_epoch=0)'),
            '/zero not permitted/')

        session_follow.close()
        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_follower_publish_epoch_not_newer_than_stable(self):
        """
        Once the follower's stable schema epoch is set, publishing a follower-created table
        at an epoch at or below it is rejected.
        """
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        self.set_stable_epoch(10, conn_follow)

        # Epoch equal to stable must fail.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.publish(self.uri, 10, session_follow),
            '/Cannot publish with a schema epoch that is older/')
        # Epoch older than stable must fail.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.publish(self.uri, 5, session_follow),
            '/Cannot publish with a schema epoch that is older/')
        # Epoch newer than stable succeeds.
        self.publish(self.uri, 20, session_follow)

        session_follow.close()
        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_follower_publish_without_epoch_noop(self):
        """
        Publish without a schema epoch on a follower-created table is a no-op: the create
        stays unpublished and is never flushed to shared metadata.
        """
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        # No schema_epoch in the config: returns success without publishing anything.
        session_follow.publish(self.uri, '')
        session_follow.close()

        self.swap_roles(conn_follow)

        self.checkpoint_and_advance(100, 2, conn_follow)
        # The create remains at the unpublished sentinel epoch, deferred past any stable epoch.
        self.assertFalse(self.uri_in_shared_metadata(conn_follow, self.uri))
        self.assertFalse(self.uri_in_local_metadata(self.conn, self.uri))

        conn_follow.close('debug=(skip_checkpoint=true)')

    #
    # Stable data in an unpublished table (fatal)
    #

    def subprocess_unpublished_stable_data(self):
        """
        Subprocess body for the unpublished-table stable-data tests; expected to panic/abort.

        Leaves an unpublished follower-created table with committed data that becomes stable,
        then checkpoints. The unpublished create is deferred past the checkpoint's stable
        epoch, so the shared metadata UPDATE for the stable data finds no CREATE entry and
        panics.
        """
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        session_follow.close()

        self.swap_roles(conn_follow)

        # Write data as the new leader so the stable constituent becomes dirty and receives a
        # real (non-fake) block checkpoint, which triggers the shared metadata UPDATE.
        session_follow = conn_follow.open_session('')
        c = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction()
        c[1] = 'value'
        session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(50))
        c.close()

        # Checkpoint with the stable timestamp covering the write: the unpublished CREATE
        # (sentinel epoch) is deferred, but the UPDATE for the now-stable data is processed,
        # finds no CREATE entry, and panics.
        self.set_stable_epoch(100, conn_follow)
        conn_follow.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(50) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        try:
            session_follow.checkpoint()  # Expected to panic.
        except wiredtiger.WiredTigerError:
            # Exit immediately to avoid hanging in tearDown when closing a panicked connection.
            os._exit(1)

    def test_unpublished_stable_data_fails(self):
        """
        Leaving an unpublished table in a state that would otherwise become stable is an
        illegal state that must fail loudly at checkpoint time.

        Checkpoint panics on this error, so the trigger is run in a subprocess.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.setup_leader_with_epoch()
        subdir = 'SUBPROCESS_unpublished_stable_data'
        [returncode, _] = self.run_subprocess_function(subdir,
            f'{self.test_name}.{self.test_name}.subprocess_unpublished_stable_data',
            silent=True)
        self.assertNotEqual(returncode, 0,
            'Expected subprocess to panic on checkpoint of stable data in an unpublished table')

    def test_unpublished_stable_data_no_partial_metadata(self):
        """
        The panicked checkpoint for stable data in an unpublished table must not leave
        partial durable metadata: the last complete checkpoint in the page log store must be
        the pre-panic checkpoint and must not contain the table.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.setup_leader_with_epoch()
        subdir = 'SUBPROCESS_no_partial_metadata'
        [returncode, new_home_dir] = self.run_subprocess_function(subdir,
            f'{self.test_name}.{self.test_name}.subprocess_unpublished_stable_data',
            silent=True)
        self.assertNotEqual(returncode, 0,
            'Expected subprocess to panic on checkpoint of stable data in an unpublished table')

        # Validate the durable state through the normal WT read path: open a follower on the
        # page log store left behind by the subprocess and pick up its last complete checkpoint.
        conn_inspect = self.open_inspection_follower(new_home_dir)

        # Only the setup checkpoint (stable timestamp 1) completed; the panicked checkpoint
        # (stable timestamp 50) produced no durable metadata for the unpublished table.
        _, _, checkpoint_timestamp, _ = self.disagg_get_complete_checkpoint_ext(conn_inspect)
        self.assertEqual(checkpoint_timestamp, 1)
        self.assertFalse(self.uri_in_shared_metadata(conn_inspect, self.uri))
        self.assertFalse(self.uri_in_local_metadata(conn_inspect, self.uri))

        conn_inspect.close('debug=(skip_checkpoint=true)')
