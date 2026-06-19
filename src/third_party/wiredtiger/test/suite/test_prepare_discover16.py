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
#
# A prepared delete that is captured by a checkpoint and later rolled
# back via the follower discover/claim flow must not affect reads of the
# underlying committed value once the rollback timestamp is stable. A
# fresh follower that opens the post-rollback checkpoint must read the
# key's original committed value.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@wttest.skip_for_hook("tiered", "Layered tables are not supported with tiered storage")
@disagg_test_class
class test_prepare_discover16(wttest.WiredTigerTestCase):
    test_name = __qualname__
    tablename = test_name
    uri = 'layered:' + tablename
    stable_uri = 'file:' + tablename + '.wt_stable'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = (
        'cache_size=10MB,statistics=(all),precise_checkpoint=true,preserve_prepared=true,')

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    def _open_follower(self, checkpoint_meta):
        conn = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' +
            self.conn_base_config + 'disaggregated=(role="follower")')
        conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        return conn

    def _checkpoint(self, conn):
        s = conn.open_session()
        s.checkpoint()
        s.close()

    def _discover_and_claim(self, conn, claim_session):
        s = conn.open_session()
        c = s.open_cursor('prepared_discover:')
        ids = []
        while c.next() == 0:
            pid = c.get_key()
            ids.append(pid)
            claim_session.begin_transaction(
                'claim_prepared_id=' + self.prepared_id_str(pid))
        c.close()
        s.close()
        return ids

    def test_rollback_clears_prepared_delete_over_committed_value(self):
        target_key = 500
        prepared_id = 17456

        # Write a committed baseline and checkpoint, so the value at
        # target_key is committed before any prepare.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        for k in range(1000):
            cursor[k] = 'committed_value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))
        self._checkpoint(self.conn)

        # Prepare a delete on target_key and checkpoint while the
        # transaction is still prepared, so the checkpoint captures the
        # prepared state of the key.
        self.session.begin_transaction()
        cursor.set_key(target_key)
        cursor.remove()
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id=' + self.prepared_id_str(prepared_id))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        self._checkpoint(self.conn)
        prepared_checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        cursor.close()
        # Close the leader without an additional checkpoint, so the
        # follower observes exactly the checkpoint with the prepared
        # state captured.
        self.conn.close('debug=(skip_checkpoint=true)')

        # On the follower, discover the prepared transaction, claim it,
        # and roll it back.
        conn_follow = self._open_follower(prepared_checkpoint_meta)
        claim_session = conn_follow.open_session()
        discovered = self._discover_and_claim(conn_follow, claim_session)
        self.assertEqual(
            discovered, [prepared_id],
            "Prepared transaction captured by the previous checkpoint "
            "must be discoverable on the follower.")
        claim_session.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(210))
        claim_session.close()

        # Step up to leader, advance stable past the rollback timestamp,
        # and take the checkpoint that must encode the rollback durably.
        conn_follow.reconfigure('disaggregated=(role="leader")')
        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        self._checkpoint(conn_follow)
        post_rollback_meta = self.disagg_get_complete_checkpoint_meta(conn_follow)
        # Close without a further checkpoint so the checkpoint chain
        # picked up below ends with the post-rollback checkpoint.
        conn_follow.close('debug=(skip_checkpoint=true)')

        # A fresh follower opens the post-rollback checkpoint and reads
        # target_key at a timestamp past the rollback. The read must
        # return the original committed value; failing the open or
        # failing the search indicates the rollback was not durable.
        try:
            conn_follow = self._open_follower(post_rollback_meta)
        except wiredtiger.WiredTigerError as e:
            self.fail(
                f"Opening the post-rollback checkpoint as a follower "
                f"failed ({e}); the rollback was not durable.")

        read_session = conn_follow.open_session()
        read_session.begin_transaction('read_timestamp=' + self.timestamp_str(300))
        # Read the stable constituent directly so the assertion does not
        # depend on the ingest constituent masking the stale state.
        read_cursor = read_session.open_cursor(self.stable_uri)
        read_cursor.set_key(target_key)
        try:
            ret = read_cursor.search()
        except wiredtiger.WiredTigerError as e:
            self.fail(
                f"Reading the rolled-back key returned an unexpected "
                f"error ({e}); the rollback was not durable.")
        self.assertEqual(
            ret, 0,
            "Reading the rolled-back key must return the original "
            "committed value, not WT_NOTFOUND.")
        self.assertEqual(read_cursor.get_value(), 'committed_value')
        read_cursor.close()
        read_session.rollback_transaction()
        conn_follow.close()
