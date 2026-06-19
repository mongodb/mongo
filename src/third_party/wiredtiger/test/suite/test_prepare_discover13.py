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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# Regression tests for HS orphans surviving step-up drain due to two
# independent issues with prepared update metadata on the ingest btree:
#   Path 1: Version cursor truncation hides discovered prepared entries.
#   Path 2: Ingest eviction drops prepared_id from resolved prepared updates.
@wttest.skip_for_hook("tiered", "Layered tables are not supported with tiered storage")
@disagg_test_class
class test_prepare_discover13(wttest.WiredTigerTestCase):
    test_name = __qualname__
    tablename = test_name
    uri = 'layered:' + tablename

    conn_base_config = (
        'cache_size=50MB,statistics=(all),precise_checkpoint=true,'
        'preserve_prepared=true,')

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    def open_follower(self, checkpoint_meta):
        """Open a follower connection in a separate directory."""
        conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' +
            self.conn_base_config + 'disaggregated=(role="follower")')
        conn_follow.reconfigure(
            f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        return conn_follow

    def create_hs_orphan_and_close_leader(self, prepare_ts):
        """
        Shared Phase 1: create an HS orphan via prepared checkpoint.
        Returns checkpoint_meta for opening a follower.
        """
        key = 1

        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        # V_base@ts=5: on-disk value so V0 becomes "newest_hs" on eviction.
        self.session.begin_transaction()
        cursor[key] = 'v_base'
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(5))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.session.checkpoint()

        # V0@ts=10: committed, stays in memory.
        self.session.begin_transaction()
        cursor[key] = 'v0'
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # V1: prepared, INPROGRESS.  Store on self so the session survives
        # until the test explicitly closes the leader connection -- otherwise
        # reference-counting destroys the local, which rolls back the
        # prepared transaction and prevents the HS orphan from forming.
        self._sess_prep = self.conn.open_session()
        cursor_prep = self._sess_prep.open_cursor(self.uri)
        self._sess_prep.begin_transaction()
        cursor_prep[key] = 'v1_prepared'
        self._sess_prep.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(prepare_ts) +
            ',prepared_id=' + self.prepared_id_str(1))
        cursor_prep.close()
        cursor.close()

        # Advance stable past prepare_ts so eviction selects V1 on-page.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(prepare_ts))

        # Evict: V0 pushed to HS with stop_ts=WT_TS_MAX (the orphan).
        evict_cursor = self.session.open_cursor(
            self.uri, None, 'debug=(release_evict)')
        self.session.begin_transaction('ignore_prepare=true')
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

        # Verify V0 was pushed to HS.
        stat_cursor = self.session.open_cursor('statistics:')
        hs_inserts = stat_cursor[wiredtiger.stat.conn.cache_hs_insert][2]
        stat_cursor.close()
        self.assertGreater(hs_inserts, 0,
            'Eviction did not push V0 to HS; orphan not created')

        return (key, prepare_ts)

    def finish_leader_checkpoint_and_close(self, key, prepare_ts, stable_ts):
        """Checkpoint at stable_ts, return checkpoint_meta, close leader."""
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(stable_ts))
        self.session.checkpoint()
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.conn.close('debug=(skip_checkpoint=true)')
        return checkpoint_meta

    def trigger_panic_via_eviction(self, conn, ts_base):
        """
        Shared final phase: write V4, V5, evict to hit the HS orphan.
        Without the fix: out-of-order timestamp PANIC.
        With the fix: clean pass.
        """
        key = 1
        sess_new = conn.open_session()
        cursor_new = sess_new.open_cursor(self.uri)

        sess_new.begin_transaction()
        cursor_new[key] = 'v4'
        sess_new.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(ts_base))

        sess_new.begin_transaction()
        cursor_new[key] = 'v5'
        sess_new.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(ts_base + 5))
        cursor_new.close()

        conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(ts_base + 5))

        evict_session = conn.open_session('debug=(release_evict_page=true)')
        evict_cursor = evict_session.open_cursor(self.uri)
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.close()
        evict_session.close()

        sess_new.close()

    # ==================================================================
    # Path 1: Version cursor truncation skips discovered entry
    # ==================================================================
    def test_path1_version_cursor_truncation(self):
        """
        Drain's version cursor truncates at the newest globally-visible
        update, never reaching the discovered entry at chain bottom.
        The HS orphan is never resolved.
        """
        key, _ = self.create_hs_orphan_and_close_leader(prepare_ts=20)

        # Checkpoint at stable_ts=20.
        # (Phase 1 already set stable to prepare_ts=20.)
        self.session.checkpoint()
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.conn.close('debug=(skip_checkpoint=true)')

        # Phase 2: Follower discover + claim.
        conn_follow = self.open_follower(checkpoint_meta)
        sess_f = conn_follow.open_session()

        discover_cursor = sess_f.open_cursor('prepared_discover:')
        claim_session = conn_follow.open_session()
        discovered = []
        while discover_cursor.next() == 0:
            pid = discover_cursor.get_key()
            discovered.append(pid)
            claim_session.begin_transaction(
                'claim_prepared_id=' + self.prepared_id_str(pid))
            claim_session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(25) +
                ',durable_timestamp=' + self.timestamp_str(26))
        discover_cursor.close()
        self.assertGreater(len(discovered), 0,
            'prepared_discover did not find V1')

        conn_follow.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(26))

        # Follower writes that become globally visible.
        cursor_f = sess_f.open_cursor(self.uri)
        sess_f.begin_transaction()
        cursor_f[key] = 'v2_follower'
        sess_f.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(30))

        sess_f.begin_transaction()
        cursor_f[key] = 'v3_follower'
        sess_f.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(35))
        cursor_f.close()

        # Advance oldest so V2/V3 become globally visible => truncation.
        conn_follow.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(35) +
            ',stable_timestamp=' + self.timestamp_str(35))

        # Step-up: drain truncates at V3, never sees V1.
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # Trigger: new writes + eviction hit the orphan.
        self.trigger_panic_via_eviction(conn_follow, ts_base=40)
        conn_follow.close()

    # ==================================================================
    # Path 2: INGEST eviction drops prepared_id
    # ==================================================================
    def test_path2_ingest_eviction_drops_prepared_id(self):
        """
        INGEST btree eviction writes V1 as a plain committed cell,
        dropping prepared_id.  Drain cannot identify V1 as a prepared
        resolution and fails to resolve the HS orphan.
        """
        key, _ = self.create_hs_orphan_and_close_leader(prepare_ts=100)

        # Checkpoint at stable_ts=150 => last_checkpoint_timestamp=150.
        checkpoint_meta = self.finish_leader_checkpoint_and_close(
            key, prepare_ts=100, stable_ts=150)

        # Phase 2: Follower discover + claim with durable_ts > last_ckpt.
        conn_follow = self.open_follower(checkpoint_meta)
        sess_f = conn_follow.open_session()

        discover_cursor = sess_f.open_cursor('prepared_discover:')
        claim_session = conn_follow.open_session()
        discovered = []
        while discover_cursor.next() == 0:
            pid = discover_cursor.get_key()
            discovered.append(pid)
            claim_session.begin_transaction(
                'claim_prepared_id=' + self.prepared_id_str(pid))
            claim_session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(200) +
                ',durable_timestamp=' + self.timestamp_str(200))
        discover_cursor.close()
        self.assertGreater(len(discovered), 0,
            'prepared_discover did not find V1')

        # Follower writes.
        cursor_f = sess_f.open_cursor(self.uri)
        sess_f.begin_transaction()
        cursor_f[key] = 'v2_follower'
        sess_f.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(250))

        sess_f.begin_transaction()
        cursor_f[key] = 'v3_follower'
        sess_f.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(300))
        cursor_f.close()

        # Phase 3: Evict the INGEST page -- the key step for Path 2.
        conn_follow.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(150) +
            ',stable_timestamp=' + self.timestamp_str(350))

        evict_session = conn_follow.open_session(
            'debug=(release_evict_page=true)')
        evict_cursor = evict_session.open_cursor(self.uri)
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()
        evict_session.close()

        # Step-up: drain returns V1 but without prepared_id.
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # Trigger: new writes + eviction hit the orphan.
        self.trigger_panic_via_eviction(conn_follow, ts_base=400)
        conn_follow.close()

if __name__ == '__main__':
    wttest.run()
