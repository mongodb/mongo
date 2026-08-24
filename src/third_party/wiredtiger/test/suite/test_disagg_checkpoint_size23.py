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

# test_disagg_checkpoint_size23.py
#   Database size accounting across the events a URI's metadata entry goes through: created,
#   checkpointed, dropped, and created again under the same name before the drop was processed.
#
#   Three values are checked together at every step, because they fail independently:
#     - the size published in the checkpoint metadata, which is what another node adopts;
#     - the maintained running size;
#     - the from-scratch recompute from the metadata, which is the definition the verify path uses.
#   A one-sided check on the published value alone cannot see an over-subtraction, and equality
#   between the maintained and recomputed sizes alone cannot see a value that never reaches a
#   checkpoint.

import re, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class

@disagg_test_class
class test_disagg_checkpoint_size23(DisaggConfigMixin, wttest.WiredTigerTestCase):

    conn_base_config = 'statistics=(all),disaggregated=(lose_all_my_data=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_follow_config = conn_base_config + 'disaggregated=(role="follower")'

    table_name = "reused_table"
    uri = "layered:" + table_name
    stable_uri = "file:" + table_name + ".wt_stable"
    table_config = 'key_format=i,value_format=S'

    # The fixed overhead every database size carries for the KEK table and shared turtle page.
    WT_DISAGG_CHECKPOINT_SIZE_BUFFER = 1024 * 1024

    def published_size(self, conn=None):
        """The database size in the newest complete checkpoint, as another node would read it."""
        match = re.search(r'database_size=(\d+)', self.disagg_get_complete_checkpoint_meta(conn))
        self.assertIsNotNone(match)
        return int(match.group(1))

    def maintained_and_recomputed(self, conn=None):
        """The running database size, and the same value summed from the metadata."""
        if conn is None:
            conn = self.conn
        local = wiredtiger.wiredtiger_repair(conn, 'fetch_database_size=(local=true)')
        recomputed = wiredtiger.wiredtiger_repair(conn, 'fetch_database_size=(local=false)')
        local = re.search(r'fetch_database_size\(local\): (\d+)', local)
        recomputed = re.search(r'fetch_database_size\(recompute\): (\d+)', recomputed)
        self.assertIsNotNone(local)
        self.assertIsNotNone(recomputed)
        return int(local.group(1)), int(recomputed.group(1))

    def stable_uri_present(self, stable_uri, conn=None):
        """Whether a stable file has a metadata entry, in the view the size walk itself reads."""
        if conn is None:
            conn = self.conn
        report = wiredtiger.wiredtiger_repair(
            conn, f'fetch_metadata=(local=true,uri="{stable_uri}")')
        return '<no matching metadata entry' not in report

    def check_consistent(self, label, conn=None):
        """The published, running and recomputed sizes must all be the same number."""
        local, recomputed = self.maintained_and_recomputed(conn)
        published = self.published_size(conn)
        self.pr(f"{label}: published={published}, local={local}, recomputed={recomputed}, "
            f"drift={local - recomputed}")
        self.assertEqual(local, recomputed,
            f"{label}: maintained database size {local} != recomputed {recomputed} "
            f"(drift {local - recomputed})")
        self.assertEqual(published, local,
            f"{label}: published database size {published} != maintained {local}")
        return local

    def populate(self, session=None, rows=1000):
        if session is None:
            session = self.session
        cursor = session.open_cursor(self.uri)
        for i in range(rows):
            cursor[i] = 'a' * 500
        cursor.close()

    # A dropped table's size must leave the database size even when the name is taken again before
    # the checkpoint processes the removal, which a recreated metadata entry under the same name
    # used to hide.
    def test_recreate_between_drops_does_not_leak(self):
        self.session.create(self.uri, self.table_config)
        self.session.checkpoint()
        size_empty = self.published_size()
        self.check_consistent("after create")

        self.populate()
        self.session.checkpoint()
        size_with_data = self.published_size()
        self.assertGreater(size_with_data, size_empty)
        self.check_consistent("after populate")

        # Take the name again before the checkpoint processes the removal, so the removal is
        # processed while the name belongs to a later table. Then drop that one too: nothing is left
        # behind, so the data's size must leave the database size.
        self.session.drop(self.uri)
        self.session.create(self.uri, self.table_config)
        self.session.checkpoint()
        self.session.drop(self.uri)
        self.session.checkpoint()
        self.session.checkpoint()

        size_after = self.published_size()
        self.pr(f"empty={size_empty}, with_data={size_with_data}, after_all_dropped={size_after}")

        # Nothing of the table may be left in the metadata. With its entry gone the recompute
        # cannot count it, so the equality check below is what proves the maintained size does not
        # either. Note the size does not return to size_empty: the shared metadata table's own
        # stable file is counted too, and the creates and drops above grew it.
        self.assertFalse(self.stable_uri_present(self.stable_uri),
            f"the dropped table's stable file must not remain: {self.stable_uri}")
        # What is left above the fixed overhead is that shared file, not the data we inserted.
        self.assertLess(size_after - self.WT_DISAGG_CHECKPOINT_SIZE_BUFFER,
            size_with_data - size_empty,
            f"the dropped data must not stay in the database size: empty={size_empty}, "
            f"with_data={size_with_data}, after_all_dropped={size_after}")
        self.check_consistent("after all dropped")

    # The same name reused repeatedly, with each removal processed while the name belongs to a later
    # table. An over-subtraction shows up as a drift between the maintained and recomputed sizes.
    def test_reuse_churn(self):
        for i in range(10):
            self.session.create(self.uri, self.table_config)
            self.populate(rows=100)
            self.session.checkpoint()

            self.session.drop(self.uri)
            self.session.create(self.uri, self.table_config)
            if i % 2 == 0:
                self.populate(rows=50)
            # A table must be checkpointed before it can be dropped.
            self.session.checkpoint()
            self.session.drop(self.uri)
            self.session.checkpoint()

            self.check_consistent(f"cycle {i}")

        self.session.checkpoint()
        self.check_consistent("after churn")

    # A node that learns of a table by adopting a checkpoint, rather than by creating it, must
    # account for the size that arrives with it: the table's metadata entry appears complete with a
    # checkpoint. Stepping the follower up makes it publish a size of its own.
    def test_size_arriving_with_a_checkpoint(self):
        self.session.create(self.uri, self.table_config)
        self.populate()
        self.session.checkpoint()
        leader_size = self.published_size()
        self.assertGreater(leader_size, self.WT_DISAGG_CHECKPOINT_SIZE_BUFFER)

        conn_follow = self.wiredtiger_open(
            'follower', self.extensionsConfig() + ',create,' + self.conn_follow_config)
        try:
            session_follow = conn_follow.open_session('')

            # Give the follower a table of its own and checkpoint it, so its size accounting is
            # already established before the pickup brings a table it has never seen.
            session_follow.create("table:follower_local", self.table_config)
            local_cursor = session_follow.open_cursor("table:follower_local")
            for i in range(100):
                local_cursor[i] = 'b' * 500
            local_cursor.close()
            session_follow.checkpoint()

            # The follower has never seen the table; the pickup brings its metadata entry, and its
            # checkpoint size along with it.
            self.disagg_advance_checkpoint(conn_follow)

            # The pickup must really have brought the table over, or the rest proves nothing.
            meta = session_follow.open_cursor('metadata:')
            meta.set_key(self.stable_uri)
            self.assertEqual(meta.search(), 0,
                "the pickup did not bring the table's stable file into the local metadata")
            meta.close()
            _, follower_recomputed = self.maintained_and_recomputed(conn_follow)
            self.assertGreater(follower_recomputed, self.WT_DISAGG_CHECKPOINT_SIZE_BUFFER,
                "the follower's metadata must account for the adopted table's data")
            self.check_consistent("follower after pickup", conn_follow)

            # Give the step up a checkpoint the follower has not already adopted.
            self.populate(rows=50)
            self.session.checkpoint()
            leader_size = self.published_size()

            # As the new leader it publishes a size, which must still cover the adopted table.
            self.disagg_switch_follower_and_leader(conn_follow)
            session_follow.checkpoint()
            follower_size = self.check_consistent("follower after step up", conn_follow)
            self.assertGreaterEqual(follower_size, leader_size,
                f"the size adopted with the checkpoint must survive a step up: leader published "
                f"{leader_size}, new leader has {follower_size}")
            session_follow.close()
        finally:
            conn_follow.close()

if __name__ == "__main__":
    wttest.run()
