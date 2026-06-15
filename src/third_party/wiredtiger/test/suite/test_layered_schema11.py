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

# test_layered_schema11.py
#   Test that checkpoint pickup on a follower does not recreate tables that
#   have been locally dropped, even when those tables still appear in the
#   shared metadata from an older checkpoint.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema11(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower",lose_all_my_data=true)'

    uri = 'layered:test_layered_schema11'
    uri2 = 'layered:test_layered_schema11_b'
    table_config = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages('test_layered_schema11', disagg_only=True)
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
        """Return True if stable_uri is present in the shared metadata table."""
        session = conn.open_session('')
        cursor = session.open_cursor('file:WiredTigerShared.wt_stable', None, None)
        cursor.set_key(stable_uri)
        found = cursor.search() == 0
        cursor.close()
        session.close()
        return found

    def assertNotInShared(self, conn, uri):
        """Assert that uri's stable constituent is absent from the shared metadata table."""
        self.assertFalse(self.uri_in_shared_metadata(conn, self.stable_uri(uri)))

    def assertInShared(self, conn, uri):
        """Assert that uri's stable constituent is present in the shared metadata table."""
        self.assertTrue(self.uri_in_shared_metadata(conn, self.stable_uri(uri)))

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

    def open_follower(self):
        """Open a follower, pick up the latest leader checkpoint, and open a session on it."""
        conn = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower)
        self.ignoreStdoutPattern('WT_VERB_RTS|(wiredtiger_open:.*WT_VERB_METADATA)')
        self.disagg_advance_checkpoint(conn)
        session = conn.open_session('')
        return conn, session

    #
    # Functional tests
    #

    def test_no_recreate_dropped_table_with_epochs(self):
        """
        A table dropped on a follower is not recreated during checkpoint pickup when
        the shared metadata still contains it and schema epochs are in use.
        """
        # Step 1: Leader creates the table and checkpoints.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 10)
        self.set_stable_epoch(10)
        self.leader_checkpoint(1)

        # Step 2: Follower picks up the checkpoint.
        conn_follow, session_follow = self.open_follower()
        self.assertInLocal(conn_follow, self.uri)

        # Step 3: Both the follower and the leader drop the table and publish the drop at
        # epoch 25. The follower's REMOVE(25) enters its local queue. The leader's REMOVE(25)
        # enters its queue but is deferred (stable epoch 10 < 25), so the table stays in
        # shared metadata for now.
        session_follow.drop(self.uri)
        self.publish(self.uri, 25, session_follow)
        self.assertNotInLocal(conn_follow, self.uri)
        self.session.drop(self.uri)
        self.publish(self.uri, 25)

        # Step 4: Leader re-checkpoints at epoch 10, before the drop epoch. The REMOVE(25) is
        # deferred; the table is still present in shared metadata.
        self.set_stable_epoch(10)
        self.leader_checkpoint(2)

        # Step 5: Follower picks up the new checkpoint. The table is in shared metadata but
        # absent locally; the REMOVE in the queue prevents recreation.
        self.disagg_advance_checkpoint(conn_follow)
        self.assertNotInLocal(conn_follow, self.uri)

        # Step 6: Leader advances past the drop epoch (epoch 25) and checkpoints. The REMOVE(25)
        # is now eligible to be flushed; the table must be removed from shared metadata.
        self.set_stable_epoch(25)
        self.leader_checkpoint(3)

        # Step 7: Follower picks up the post-drop checkpoint. The table must be absent from
        # both shared and local metadata.
        self.disagg_advance_checkpoint(conn_follow)
        self.assertNotInShared(conn_follow, self.uri)
        self.assertNotInLocal(conn_follow, self.uri)

        session_follow.close()
        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_no_recreate_dropped_table_isolation(self):
        """Dropping one table must not prevent pickup of a second table that still exists."""
        # Step 1: Leader creates both tables and checkpoints at epoch 10.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 10)
        self.session.create(self.uri2, self.table_config)
        self.publish(self.uri2, 10)
        self.set_stable_epoch(10)
        self.leader_checkpoint(1)

        # Step 2: Follower picks up the checkpoint; both tables are in local metadata.
        conn_follow, session_follow = self.open_follower()
        self.assertInLocal(conn_follow, self.uri)
        self.assertInLocal(conn_follow, self.uri2)

        # Step 3: Both follower and leader drop uri at epoch 25; uri2 is kept.
        session_follow.drop(self.uri)
        self.publish(self.uri, 25, session_follow)
        self.assertNotInLocal(conn_follow, self.uri)
        self.assertInLocal(conn_follow, self.uri2)
        self.session.drop(self.uri)
        self.publish(self.uri, 25)

        # Step 4: Leader re-checkpoints at epoch 10 (REMOVE deferred; both tables still in
        # shared metadata).
        self.set_stable_epoch(10)
        self.leader_checkpoint(2)

        # Step 5: Follower picks up; uri must not be recreated, uri2 must still be present.
        self.disagg_advance_checkpoint(conn_follow)
        self.assertNotInLocal(conn_follow, self.uri)
        self.assertInLocal(conn_follow, self.uri2)

        # Step 6: Leader advances to epoch 25 and checkpoints; uri is removed from shared metadata.
        self.set_stable_epoch(25)
        self.leader_checkpoint(3)

        # Step 7: Follower picks up; uri absent from both shared and local; uri2 intact.
        self.disagg_advance_checkpoint(conn_follow)
        self.assertNotInShared(conn_follow, self.uri)
        self.assertNotInLocal(conn_follow, self.uri)
        self.assertInLocal(conn_follow, self.uri2)

        session_follow.close()
        conn_follow.close('debug=(skip_checkpoint=true)')

    def test_recreate_after_drop_then_create(self):
        """
        When the queue contains REMOVE followed by CREATE for the same table, the latest entry
        is CREATE, so the table must be picked up on the next checkpoint (not blocked by the
        REMOVE).
        """
        # Step 1: Leader creates uri and checkpoints at epoch 10.
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 10)
        self.set_stable_epoch(10)
        self.leader_checkpoint(1)

        # Step 2: Follower picks up; uri is in local metadata.
        conn_follow, session_follow = self.open_follower()
        self.assertInLocal(conn_follow, self.uri)

        # Step 3: Both follower and leader drop uri at epoch 25 (REMOVE(25) queued), then
        # re-create it at epoch 40 (CREATE(40) queued). The latest queue entry for uri is now
        # CREATE.
        session_follow.drop(self.uri)
        self.publish(self.uri, 25, session_follow)
        session_follow.create(self.uri, self.table_config)
        self.publish(self.uri, 40, session_follow)
        self.session.drop(self.uri)
        self.publish(self.uri, 25)
        self.session.create(self.uri, self.table_config)
        self.publish(self.uri, 40)

        # Step 4: Leader checkpoints at epoch 10; both ops deferred. Shared metadata still has
        # the original entry from step 1.
        self.set_stable_epoch(10)
        self.leader_checkpoint(2)

        # Step 5: Follower picks up. Because the latest queue entry is CREATE(40) (not REMOVE),
        # the REMOVE check does not block pickup; uri must be present in local metadata.
        self.disagg_advance_checkpoint(conn_follow)
        self.assertInLocal(conn_follow, self.uri)

        session_follow.close()
        conn_follow.close('debug=(skip_checkpoint=true)')
