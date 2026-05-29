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

# test_layered_schema10.py
#   Test the publish API on followers and step-up behavior.
#
#   Schema operations (create, drop) queued on a follower are replayed during
#   step-up, which uses the metadata operation queue populated while the node
#   was a follower.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema10(wttest.WiredTigerTestCase, suite_subprocess):
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = 'layered:test_layered_schema10'
    uri2 = 'layered:test_layered_schema10_2'  # second follower-created table for multi-epoch tests

    table_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages('test_layered_schema10', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    #
    # Helper methods
    #

    def set_stable_epoch(self, epoch, conn=None):
        if conn is None:
            conn = self.conn
        conn.set_timestamp(
            'stable_disaggregated_schema_epoch=' + self.timestamp_str(epoch))

    def leader_checkpoint(self, stable_ts, conn=None, session=None):
        if conn is None:
            conn = self.conn
        if session is None:
            session = self.session
        conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(stable_ts) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        session.checkpoint()

    def publish(self, uri, epoch, session=None):
        if session is None:
            session = self.session
        session.publish(uri, 'disaggregated=(schema_epoch=' + self.timestamp_str(epoch) + ')')

    def stable_uri(self, uri):
        """Return the stable component URI for a given layered table URI."""
        tablename = uri[len('layered:'):]
        return 'file:' + tablename + '.wt_stable'

    def uri_in_shared_metadata(self, conn, stable_uri):
        """
        Return True if stable_uri is present in the shared metadata table.
        """
        session = conn.open_session('')
        cursor = session.open_cursor('file:WiredTigerShared.wt_stable', None, None)
        cursor.set_key(stable_uri)
        found = cursor.search() == 0
        cursor.close()
        session.close()
        return found

    def uri_in_local_metadata(self, conn, uri):
        """Return True if uri is present in the local metadata (cursor open succeeds)."""
        session = conn.open_session('')
        exists = True
        try:
            c = session.open_cursor(uri)
            c.close()
        except wiredtiger.WiredTigerError:
            exists = False
        session.close()
        return exists

    def assertInLocal(self, conn, uri):
        """Assert that uri's stable constituent is present in conn's local metadata."""
        self.assertTrue(self.uri_in_local_metadata(conn, self.stable_uri(uri)))

    def assertNotInLocal(self, conn, uri):
        """Assert that uri's stable constituent is absent from conn's local metadata."""
        self.assertFalse(self.uri_in_local_metadata(conn, self.stable_uri(uri)))

    def assertInShared(self, conn, uri):
        """Assert that uri's stable constituent is present in the shared metadata table."""
        self.assertTrue(self.uri_in_shared_metadata(conn, self.stable_uri(uri)))

    def assertNotInShared(self, conn, uri):
        """Assert that uri's stable constituent is absent from the shared metadata table."""
        self.assertFalse(self.uri_in_shared_metadata(conn, self.stable_uri(uri)))

    def setup_leader_with_epoch(self):
        """
        Seed the shared storage with a checkpoint carrying schema epoch 10.

        A non-zero schema epoch causes a follower picking up this checkpoint
        to use the epoch-based step-up code path rather than the legacy fallback.
        """
        self.set_stable_epoch(10)
        self.leader_checkpoint(1)

    def swap_roles(self, conn_follower):
        """
        Swap roles: demote self.conn (leader to follower) and promote conn_follower (follower to leader).

        We reconfigure directly without passing checkpoint_meta to avoid triggering
        a re-pickup of the same checkpoint that the follower already has.
        """
        self.conn.reconfigure('disaggregated=(role="follower")')
        conn_follower.reconfigure('disaggregated=(role="leader")')

    def open_follower(self):
        """Open a follower, pick up the latest leader checkpoint, and open a session on it."""
        conn = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower)
        self.ignoreStdoutPattern('WT_VERB_RTS|(wiredtiger_open:.*WT_VERB_METADATA)')
        self.disagg_advance_checkpoint(conn)
        session = conn.open_session('')
        return conn, session

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

    #
    # Functional tests
    #

    def test_create_on_follower_step_up(self):
        """A table created and published on a follower is accessible after a role swap."""
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        self.publish(self.uri, 20, session_follow)
        session_follow.close()

        # Pre-swap state:
        # Shared metadata: empty (no schema operations on the initial leader).
        # Follower: uri layered table present; metadata queue holds CREATE uri at epoch 20.
        self.assertNotInLocal(conn_follow, self.uri)
        self.swap_roles(conn_follow)

        # After step-up: uri stable constituent created locally; shared metadata unchanged.
        self.assertNotInShared(conn_follow, self.uri)
        self.assertInLocal(conn_follow, self.uri)

        self.checkpoint_and_advance(15, 2, conn_follow)
        # After checkpoint at epoch=15: CREATE (epoch=20) deferred; uri absent from self.conn.
        self.assertNotInLocal(self.conn, self.uri)

        self.checkpoint_and_advance(20, 3, conn_follow)
        # After checkpoint at epoch=20: CREATE flushed; uri's stable constituent visible to self.conn.
        self.assertInLocal(self.conn, self.uri)

        session_follow = conn_follow.open_session('')
        c = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction()
        c[1] = 'after_step_up'
        session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(100))
        c.close()
        session_follow.close()

        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_create_drop_on_follower_step_up(self):
        """A table created then dropped on a follower must not exist after a role swap."""
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        self.publish(self.uri, 20, session_follow)
        session_follow.drop(self.uri)
        self.publish(self.uri, 30, session_follow)
        session_follow.close()

        # Pre-swap state:
        # Shared metadata: empty (no schema operations on the initial leader).
        # Follower: uri was created then immediately dropped; the layered table no longer
        #   exists; no stable constituent was ever created (skipped on create, moot on drop);
        #   queue holds CREATE uri (epoch 20) then REMOVE uri (epoch 30).
        self.assertNotInLocal(conn_follow, self.uri)
        self.swap_roles(conn_follow)

        # After step-up: net create+drop leaves no trace in either metadata store.
        self.assertNotInShared(conn_follow, self.uri)
        self.assertNotInLocal(conn_follow, self.uri)

        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_create_on_follower_visible_after_checkpoint(self):
        """
        After a role swap and a new leader checkpoint, uri's stable table is visible
        to the old leader (now a follower) after it picks up the new checkpoint.
        """
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        self.publish(self.uri, 20, session_follow)

        c = session_follow.open_cursor(self.uri)
        session_follow.begin_transaction()
        c[1] = 'follower_value'
        session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(50))
        c.close()
        session_follow.close()

        # Pre-swap state:
        # Shared metadata: empty on both the original leader and the follower (no schema
        #   operations on the initial leader; uri created on the follower, never checkpointed).
        # Follower: uri layered table present with committed data; metadata queue holds
        #   CREATE uri at epoch 20.
        self.assertNotInShared(self.conn, self.uri)
        self.assertNotInLocal(conn_follow, self.uri)
        self.swap_roles(conn_follow)

        # After step-up: uri stable constituent created locally; shared metadata unchanged.
        # self.conn has not yet picked up any new checkpoint.
        self.assertNotInShared(conn_follow, self.uri)
        self.assertInLocal(conn_follow, self.uri)
        self.assertNotInShared(self.conn, self.uri)
        self.assertNotInLocal(self.conn, self.uri)

        self.checkpoint_and_advance(20, 100, conn_follow)
        # After checkpoint at epoch=20: CREATE flushed; conn_follow (leader) sees the update
        # in shared metadata; self.conn (follower) sees it via local metadata after pickup.
        self.assertInShared(conn_follow, self.uri)
        self.assertInLocal(self.conn, self.uri)

        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_multiple_tables_different_epochs(self):
        """Tables published at different epochs are flushed to shared metadata independently."""
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        session_follow.create(self.uri2, self.table_config)
        self.publish(self.uri, 20, session_follow)
        self.publish(self.uri2, 30, session_follow)
        session_follow.close()

        # Pre-swap state:
        # Shared metadata: empty (no schema operations on the initial leader).
        # Follower: uri and uri2 layered tables present; queue holds CREATE uri (epoch 20)
        #   and CREATE uri2 (epoch 30).
        self.assertNotInLocal(conn_follow, self.uri)
        self.assertNotInLocal(conn_follow, self.uri2)
        self.swap_roles(conn_follow)

        # After step-up: both stable constituents created locally.
        self.assertInLocal(conn_follow, self.uri)
        self.assertInLocal(conn_follow, self.uri2)

        self.checkpoint_and_advance(20, 2, conn_follow)
        # After checkpoint at epoch=20: CREATE uri flushed; CREATE uri2 (epoch=30) deferred.
        self.assertInLocal(self.conn, self.uri)
        self.assertNotInLocal(self.conn, self.uri2)

        self.checkpoint_and_advance(30, 3, conn_follow)
        # After checkpoint at epoch=30: CREATE uri2 flushed; both tables visible to self.conn.
        self.assertInLocal(self.conn, self.uri)
        self.assertInLocal(self.conn, self.uri2)

        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_drop_on_follower_step_up(self):
        """
        A table dropped on a follower is removed from shared metadata only at the checkpoint
        whose stable epoch reaches the published drop epoch.
        """
        self.setup_leader_with_epoch()

        # Create uri on the initial leader at epoch 15.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 15)
        self.set_stable_epoch(15)
        self.leader_checkpoint(2)

        conn_follow, session_follow = self.open_follower()

        # Drop uri on the follower and publish the drop at epoch 25.
        # The REMOVE queue entry captures the current metadata values before deletion.
        session_follow.drop(self.uri)
        self.publish(self.uri, 25, session_follow)
        session_follow.close()

        # Pre-swap state:
        # Shared metadata: uri (epoch 15), from the leader checkpoint; the follower picked it up.
        # Follower: uri was dropped; queue holds REMOVE uri at epoch 25.
        self.assertInShared(conn_follow, self.uri)
        self.swap_roles(conn_follow)

        # After step-up: REMOVE queued; shared metadata still reflects the last checkpoint.
        self.assertInShared(conn_follow, self.uri)

        self.checkpoint_and_advance(20, 3, conn_follow)
        # After checkpoint at epoch=20: REMOVE (epoch=25) deferred; uri still in shared metadata.
        self.assertInShared(conn_follow, self.uri)

        self.checkpoint_and_advance(25, 4, conn_follow)
        # After checkpoint at epoch=25: REMOVE flushed; uri gone from shared metadata.
        self.assertNotInShared(conn_follow, self.uri)

        conn_follow.close('debug=(skip_checkpoint=true)')

    def subprocess_create_drop_split_epochs(self):
        """Subprocess body for the split-epochs panic test; expected to panic/abort."""
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        self.publish(self.uri, 20, session_follow)
        session_follow.drop(self.uri)
        self.publish(self.uri, 30, session_follow)
        session_follow.close()

        # Pre-swap state:
        # Shared metadata: empty (no schema operations on the initial leader).
        # Follower: uri was created then dropped; queue holds CREATE uri (epoch 20) followed
        #   by REMOVE uri (epoch 30).
        self.swap_roles(conn_follow)

        # After step-up: CREATE followed by REMOVE causes step-up to skip stable constituent
        # creation, leaving no local trace.
        self.assertNotInLocal(conn_follow, self.uri)
        self.assertNotInShared(conn_follow, self.uri)

        # Checkpoint at epoch=20: the stable epoch falls between CREATE (epoch=20) and
        # DROP (epoch=30), so the table must be visible in shared metadata at this checkpoint.
        # But the stable constituent was never created, so WiredTiger panics.
        self.set_stable_epoch(20, conn_follow)
        conn_follow.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(2) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        session_ck = conn_follow.open_session('')
        session_ck.checkpoint()  # Expected to panic.

    def test_create_drop_split_epochs(self):
        """
        Publishing CREATE and DROP at different epochs is an API violation that panics.

        The stable epoch falls between CREATE (epoch=20) and DROP (epoch=30), so this checkpoint
        must include the table in shared metadata. But the table was dropped and its stable
        constituent was never created, so we have no data to write. WiredTiger detects this and
        panics.
        """
        # Initialize self.conn so the test fixture can close it cleanly; the real test runs
        # in a subprocess so that the panic/abort does not kill the test runner.
        self.setup_leader_with_epoch()
        subdir = 'SUBPROCESS_create_drop_split_epochs'
        [returncode, _] = self.run_subprocess_function(subdir,
            'test_layered_schema10.test_layered_schema10.subprocess_create_drop_split_epochs',
            silent=True)
        self.assertNotEqual(returncode, 0)

    def test_unpublished_create_not_flushed(self):
        """
        A table created on a follower but never published does not appear in shared metadata
        after a role swap and leader checkpoints.
        """
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        # Create uri but do not call publish.
        session_follow.create(self.uri, self.table_config)
        session_follow.close()

        # Pre-swap state:
        # Shared metadata: empty (no schema operations on the initial leader).
        # Follower: uri layered table present; metadata queue holds CREATE uri with the
        #   unpublished sentinel epoch, which is deferred past any finite stable schema epoch.
        self.assertNotInLocal(conn_follow, self.uri)
        self.swap_roles(conn_follow)

        # After step-up: stable constituent created locally; shared metadata unchanged
        # (and will never contain uri because the sentinel epoch can never be reached).
        self.assertInLocal(conn_follow, self.uri)
        self.assertNotInShared(conn_follow, self.uri)

        self.checkpoint_and_advance(20, 2, conn_follow)
        # After checkpoint at epoch=20: unpublished CREATE deferred; uri absent from self.conn.
        self.assertNotInLocal(self.conn, self.uri)

        self.checkpoint_and_advance(100, 3, conn_follow)
        # After checkpoint at epoch=100: still deferred; uri absent from self.conn.
        self.assertNotInLocal(self.conn, self.uri)

        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_epoch_advance_alone_prevents_checkpoint_skip(self):
        """
        A checkpoint must not be skipped when only the stable schema epoch advances.
        """
        self.setup_leader_with_epoch()

        conn_follow, session_follow = self.open_follower()

        session_follow.create(self.uri, self.table_config)
        self.publish(self.uri, 20, session_follow)
        session_follow.close()

        # Pre-swap state:
        # Shared metadata: empty (no schema operations on the initial leader).
        # Follower: uri layered table present; metadata queue holds CREATE uri at epoch 20.
        self.swap_roles(conn_follow)

        session_follow = conn_follow.open_session('')

        self.set_stable_epoch(15, conn_follow)
        conn_follow.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(2) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        session_follow.checkpoint()
        self.disagg_advance_checkpoint(self.conn, conn_follow)
        # After checkpoint at epoch=15: CREATE (epoch=20) deferred; uri absent from self.conn.
        self.assertNotInLocal(self.conn, self.uri)

        # Advance ONLY the stable schema epoch to 20; stable_timestamp stays at 2.
        # The checkpoint must NOT be skipped because the schema epoch changed, even though no
        # transactional data changed and the stable timestamp is unchanged.
        self.set_stable_epoch(20, conn_follow)
        session_follow.checkpoint()
        self.disagg_advance_checkpoint(self.conn, conn_follow)
        # After checkpoint at epoch=20: CREATE flushed; uri visible to self.conn.
        self.assertInLocal(self.conn, self.uri)

        session_follow.close()
        conn_follow.close('debug=(skip_checkpoint=true)')
