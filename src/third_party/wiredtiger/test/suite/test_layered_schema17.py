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

# Test the disaggregated=(strict_checkpoint_metadata) connection option.
#
# When strict mode is on, checkpoint pickup panics if a layered table is
# present in only one of the local and shared metadata and the difference is
# not explained by a pending metadata-queue CREATE/REMOVE with a schema epoch
# greater than the picked-up checkpoint's schema epoch.
#
# Strict mode is enabled on the follower only after its startup pickup (the
# startup pickup populates an empty local metadata from the checkpoint, which
# strict mode would reject by design). Subsequent pickups pass only
# checkpoint_meta to reconfigure, so they also exercise the flag's stickiness.

import os
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema17(wttest.WiredTigerTestCase, suite_subprocess, DisaggSchemaEpochMixin):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = f'layered:{test_name}'
    uri2 = f'layered:{test_name}_2'

    table_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    #
    # Helper methods
    #

    def setup_leader_empty(self):
        """Seed the shared storage with a checkpoint at schema epoch 10, no tables."""
        self.set_stable_epoch(10)
        self.leader_checkpoint(1)

    def setup_leader_with_table(self):
        """Seed the shared storage with a checkpoint at schema epoch 20 containing uri."""
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 20)
        self.set_stable_epoch(20)
        self.leader_checkpoint(1)

    def open_strict_follower(self):
        """
        Open a follower (performing the non-strict startup pickup) and then enable
        strict checkpoint metadata validation for all subsequent pickups.
        """
        conn_follow, session_follow = self.open_follower()
        conn_follow.reconfigure('disaggregated=(strict_checkpoint_metadata=true)')
        return conn_follow, session_follow

    def leader_checkpoint_at_epoch(self, epoch, stable_ts):
        """Advance the leader's stable schema epoch and take a new checkpoint."""
        self.set_stable_epoch(epoch)
        self.leader_checkpoint(stable_ts)

    def uri_layered_in_local_metadata(self, conn, uri):
        """
        Return True if the layered: entry is present in conn's local metadata.

        Unlike uri_in_local_metadata, this does not rely on the stable constituent,
        which follower-created tables lack until step-up.
        """
        session = conn.open_session('')
        cursor = session.open_cursor('metadata:')
        cursor.set_key(uri)
        found = cursor.search() == 0
        cursor.close()
        session.close()
        return found

    def run_panic_subprocess(self, name):
        """
        Run subprocess_<name> in a subprocess and assert it died from the strict
        checkpoint metadata validation panic (not from an unrelated failure).
        """
        [returncode, home] = self.run_subprocess_function(f'SUBPROCESS_{name}',
            f'{self.test_name}.{self.test_name}.subprocess_{name}', silent=True)
        self.assertNotEqual(returncode, 0)
        self.check_file_contains(os.path.join(home, 'stderr.txt'),
            'strict checkpoint metadata validation failed')

    #
    # Positive tests: explained (or no) differences do not panic under strict mode.
    #

    def test_strict_matched_metadata(self):
        """A pickup with identical local and shared table sets passes strict validation."""
        self.setup_leader_with_table()

        conn_follow, session_follow = self.open_strict_follower()
        session_follow.close()

        # The startup pickup created uri locally, so the next pickup sees matched sets.
        self.assertTrue(self.uri_in_local_metadata(conn_follow, self.uri))

        self.leader_checkpoint_at_epoch(21, 2)
        # The advance passes only checkpoint_meta, so strict mode must remain on (sticky).
        self.disagg_advance_checkpoint(conn_follow)

        self.assertTrue(self.uri_in_shared_metadata(conn_follow, self.uri))
        self.assertTrue(self.uri_in_local_metadata(conn_follow, self.uri))

        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_strict_unpublished_local_table(self):
        """
        A created-but-unpublished follower table is a local-only difference explained by
        its queued CREATE at the unpublished sentinel epoch, which exceeds any checkpoint epoch.
        """
        self.setup_leader_with_table()

        conn_follow, session_follow = self.open_strict_follower()
        session_follow.create(self.uri2, self.table_config)
        session_follow.close()

        self.leader_checkpoint_at_epoch(30, 2)
        self.disagg_advance_checkpoint(conn_follow)

        # uri2 survives locally and never reaches shared metadata.
        self.assertTrue(self.uri_layered_in_local_metadata(conn_follow, self.uri2))
        self.assertFalse(self.uri_in_shared_metadata(conn_follow, self.uri2))

        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_strict_future_create_ok(self):
        """
        A follower table published at an epoch greater than the picked-up checkpoint's
        epoch is a local-only difference explained by its pending CREATE.
        """
        self.setup_leader_with_table()

        conn_follow, session_follow = self.open_strict_follower()
        session_follow.create(self.uri2, self.table_config)
        self.publish(self.uri2, 40, session_follow)
        session_follow.close()

        # Checkpoint epoch 30 < CREATE epoch 40: the difference is explained.
        self.leader_checkpoint_at_epoch(30, 2)
        self.disagg_advance_checkpoint(conn_follow)

        self.assertTrue(self.uri_layered_in_local_metadata(conn_follow, self.uri2))
        self.assertFalse(self.uri_in_shared_metadata(conn_follow, self.uri2))

        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_strict_pending_remove_ok(self):
        """
        A follower drop published at an epoch greater than the picked-up checkpoint's
        epoch is a shared-only difference explained by its pending REMOVE; the pickup
        must not resurrect the table locally.
        """
        self.setup_leader_with_table()

        conn_follow, session_follow = self.open_strict_follower()
        session_follow.drop(self.uri)
        self.publish(self.uri, 40, session_follow)
        session_follow.close()

        # Checkpoint epoch 30 < REMOVE epoch 40; the checkpoint still contains uri.
        self.leader_checkpoint_at_epoch(30, 2)
        self.disagg_advance_checkpoint(conn_follow)

        self.assertTrue(self.uri_in_shared_metadata(conn_follow, self.uri))
        self.assertFalse(self.uri_layered_in_local_metadata(conn_follow, self.uri))
        self.assertFalse(self.uri_in_local_metadata(conn_follow, self.uri))

        conn_follow.close('debug=(skip_checkpoint=true)')

    #
    # Negative tests: unexplained differences panic. Each panic scenario runs in a
    # subprocess because the panic aborts the process in diagnostic builds.
    #

    def subprocess_shared_only_panics(self):
        """Subprocess body for the shared-only panic test; expected to panic/abort."""
        self.setup_leader_empty()

        conn_follow, session_follow = self.open_strict_follower()
        session_follow.close()

        # Create uri2 on the leader only; the follower has no queue entry for it.
        self.session.create(self.uri2, self.table_config)
        self.publish(self.uri2, 30)
        self.leader_checkpoint_at_epoch(30, 2)

        # uri2 is in shared metadata only and the follower never applied the schema
        # operation that created it, so nothing explains the difference. Expected to panic.
        self.disagg_advance_checkpoint(conn_follow)

    def test_strict_shared_only_panics(self):
        """
        A table present in shared metadata only, with no pending queue entry on the
        follower, is an unexplained difference: strict pickup panics.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.setup_leader_empty()
        self.run_panic_subprocess('shared_only_panics')

    def subprocess_stale_create_panics(self):
        """Subprocess body for the stale-create panic test; expected to panic/abort."""
        # Seed the leader with uri: pickup applies the shared metadata (and hence
        # validates) only when the shared metadata table has been checkpointed.
        self.setup_leader_with_table()

        conn_follow, session_follow = self.open_strict_follower()

        # Publish the follower's CREATE at epoch 30, then pick up a checkpoint at
        # epoch 40: the CREATE should already be reflected in that checkpoint, so the
        # table being local-only is unexplained.
        session_follow.create(self.uri2, self.table_config)
        self.publish(self.uri2, 30, session_follow)
        session_follow.close()

        self.leader_checkpoint_at_epoch(40, 2)

        self.disagg_advance_checkpoint(conn_follow)  # Expected to panic.

    def test_strict_stale_create_panics(self):
        """
        A local-only table whose latest queued CREATE has an epoch at or below the
        picked-up checkpoint's epoch is an unexplained difference: strict pickup panics.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.setup_leader_empty()
        self.run_panic_subprocess('stale_create_panics')

    def subprocess_stale_remove_panics(self):
        """Subprocess body for the stale-remove panic test; expected to panic/abort."""
        self.setup_leader_with_table()

        conn_follow, session_follow = self.open_strict_follower()

        # Publish the follower's drop of uri at epoch 30, then pick up a checkpoint at
        # epoch 40: the REMOVE should already be reflected in that checkpoint, so uri
        # still being in shared metadata is unexplained. (The leader never dropped uri,
        # so its checkpoint still contains it.)
        session_follow.drop(self.uri)
        self.publish(self.uri, 30, session_follow)
        session_follow.close()

        self.leader_checkpoint_at_epoch(40, 2)

        self.disagg_advance_checkpoint(conn_follow)  # Expected to panic.

    def test_strict_stale_remove_panics(self):
        """
        A shared-only table whose latest queued REMOVE has an epoch at or below the
        picked-up checkpoint's epoch is an unexplained difference: strict pickup panics.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.setup_leader_empty()
        self.run_panic_subprocess('stale_remove_panics')

    def subprocess_strict_at_open_panics(self):
        """Subprocess body for the strict-at-open panic test; expected to panic/abort."""
        self.setup_leader_with_table()

        # Open a follower with strict mode already enabled in the open config. Its
        # clean-startup pickup finds uri in the shared metadata with nothing in the
        # empty local metadata or queue to explain it.
        conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,' + self.conn_base_config +
            'disaggregated=(role="follower",lose_all_my_data=true,' +
            'strict_checkpoint_metadata=true)')
        self.disagg_advance_checkpoint(conn_follow)  # Expected to panic.

    def test_strict_at_open_panics(self):
        """
        Strict validation enabled in the open config panics on the clean-startup
        pickup: an empty node's differences from the checkpoint are unexplained by
        design, which is why the mode must be enabled only after the startup pickup.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.setup_leader_empty()
        self.run_panic_subprocess('strict_at_open_panics')

    #
    # Step-down tests: stepping down clears the metadata queue, so a table created
    # but never published before the step-down becomes an unexplained local-only
    # difference. The application must drop such tables (or disable strict mode)
    # before the stepped-down node installs checkpoints.
    #

    def step_down_with_unpublished_table(self):
        """
        Create an unpublished table on the leader, then swap roles with a fresh
        follower and have the new leader take a checkpoint at a higher epoch.
        Returns the new leader's connection.
        """
        self.setup_leader_with_table()
        conn_follow, session_follow = self.open_follower()
        session_follow.close()

        # An operation that was rolled back leaves a created-but-unpublished table
        # behind, waiting for a drop.
        self.session.create(self.uri2, self.table_config)

        # Step down; the metadata queue is cleared, so uri2's CREATE no longer
        # explains its presence in the local metadata.
        self.conn.reconfigure('disaggregated=(role="follower")')
        conn_follow.reconfigure('disaggregated=(role="leader")')

        session_lead = conn_follow.open_session('')
        self.set_stable_epoch(30, conn_follow)
        self.leader_checkpoint(2, conn_follow, session_lead)
        session_lead.close()
        return conn_follow

    def subprocess_step_down_pending_create_panics(self):
        """Subprocess body for the step-down panic test; expected to panic/abort."""
        conn_lead = self.step_down_with_unpublished_table()

        self.conn.reconfigure('disaggregated=(strict_checkpoint_metadata=true)')
        self.disagg_advance_checkpoint(self.conn, conn_lead)  # Expected to panic.

    def test_strict_step_down_pending_create_panics(self):
        """
        A node that steps down with a created-but-unpublished table cannot pass
        strict validation: the queue was cleared on step-down, so the local-only
        table is unexplained and picking up the new leader's checkpoint panics.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.setup_leader_empty()
        self.run_panic_subprocess('step_down_pending_create_panics')

    def test_strict_step_down_drop_then_pickup(self):
        """
        Completing the pending drop of the unpublished table resolves the step-down
        inconsistency: once the table is gone from the local metadata, strict
        validation passes and the stepped-down node can install checkpoints.
        """
        conn_lead = self.step_down_with_unpublished_table()

        self.session.drop(self.uri2)
        self.conn.reconfigure('disaggregated=(strict_checkpoint_metadata=true)')
        self.disagg_advance_checkpoint(self.conn, conn_lead)

        self.assertTrue(self.uri_layered_in_local_metadata(self.conn, self.uri))
        self.assertFalse(self.uri_layered_in_local_metadata(self.conn, self.uri2))
        self.assertFalse(self.uri_in_shared_metadata(self.conn, self.uri2))

        conn_lead.close('debug=(skip_checkpoint=true)')

    #
    # Flag semantics tests.
    #

    def test_nonstrict_unchanged(self):
        """
        Without strict mode, the shared-only divergence of the panic scenario keeps
        today's behavior: the pickup creates the table locally.
        """
        self.setup_leader_empty()

        conn_follow, session_follow = self.open_follower()
        session_follow.close()

        self.session.create(self.uri2, self.table_config)
        self.publish(self.uri2, 30)
        self.leader_checkpoint_at_epoch(30, 2)

        self.disagg_advance_checkpoint(conn_follow)

        self.assertTrue(self.uri_in_shared_metadata(conn_follow, self.uri2))
        self.assertTrue(self.uri_in_local_metadata(conn_follow, self.uri2))

        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_explicit_false_disables(self):
        """
        An explicit strict_checkpoint_metadata=false turns strict mode back off: the
        shared-only divergence of the panic scenario no longer panics and the table
        is created locally on pickup.
        """
        self.setup_leader_empty()

        conn_follow, session_follow = self.open_strict_follower()
        session_follow.close()

        self.session.create(self.uri2, self.table_config)
        self.publish(self.uri2, 30)
        self.leader_checkpoint_at_epoch(30, 2)

        conn_follow.reconfigure('disaggregated=(strict_checkpoint_metadata=false)')
        self.disagg_advance_checkpoint(conn_follow)

        self.assertTrue(self.uri_in_shared_metadata(conn_follow, self.uri2))
        self.assertTrue(self.uri_in_local_metadata(conn_follow, self.uri2))

        conn_follow.close('debug=(skip_checkpoint=true)')
