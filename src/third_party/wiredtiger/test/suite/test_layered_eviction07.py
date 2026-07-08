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

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered_eviction07.py
#    A leader that dirties a page after its last checkpoint, then steps down,
#    leaves a dirty resident page on a btree that step-down marks read-only. If
#    eviction later picks that page it routes into reconciliation and hits the
#    "Attempting reconciliation on a read-only page" assertion. The dirty state
#    belongs to an abandoned epoch and should be discarded, not reconciled.
@disagg_test_class
class test_layered_eviction07(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,disaggregated=(lose_all_my_data=true),'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=S,value_format=S'

    table_name = "test_layered_eviction07"

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, [
        # Use the shared (disaggregated) table directly so that step-down marks
        # this btree read-only.
        ('shared', dict(prefix='table:', table_config='block_manager=disagg,log=(enabled=false)')),
    ])

    def test_layered_eviction07(self):
        # Avoid checkpoint error with precise checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))

        # The node started as a follower, so step it up as the leader.
        self.conn.reconfigure('disaggregated=(role="leader")')

        self.uri = self.prefix + self.table_name
        self.session.create(self.uri, self.create_session_config + ',' + self.table_config)

        # Insert a row and checkpoint so the page has a materialized image in
        # shared storage. After this the resident page is clean.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['a'] = 'b'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Dirty the same page again, above the last checkpoint timestamp, and do
        # NOT checkpoint. This update belongs to the current leader epoch and is
        # never flushed to shared storage.
        #
        # Keep this cursor OPEN across step-down. An open cursor pins its data
        # handle's in-use count above zero for its whole lifetime. Committing the
        # transaction resets the cursor (it drops its hazard pointer on the page,
        # so eviction is not blocked) but does not close it, so the pin survives.
        # That pin is the whole point: when eviction later opens a handle for this
        # table, the in-use count keeps it bound to this now read-only handle
        # instead of being rerouted to a freshly opened one.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['a'] = 'c'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Make 'c' stable (but still NOT checkpointed) so it is durable-eligible
        # and visible to a follower read. The page stays dirty with 'c' resident.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Step down to follower. This marks the btree read-only and the dhandle
        # outdated; it deliberately does not checkpoint, so the dirty page stays
        # resident. The still-open cursor keeps the in-use count above zero, so
        # the read-only handle is neither swept nor bypassed by handle lookup.
        self.conn.reconfigure('disaggregated=(role="follower")')

        # Step back up to leader. The stale handle keeps its read-only/outdated
        # flags (read-only is never cleared), but the connection is a leader
        # again. This is the production shape from the core dump: step-down marks
        # the old handle read-only/outdated, a later step-up opens a fresh
        # writable handle, and eviction then reaches the dirty page on the stale
        # handle while the node is a leader. Being a leader passes the eviction
        # leader-gate assertion and proceeds into reconciliation, where the still
        # read-only btree trips the read-only assertion.
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Force eviction of the now read-only page. Because the leader handle is
        # still pinned in use, this cursor binds to it (not a freshly opened
        # handle) and positions on the resident dirty page; reading back 'c'
        # confirms we are on the dirty handle rather than the checkpointed image.
        # The reset then drives release_eviction on the dirty read-only page.
        self.session.begin_transaction()
        evict_cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        evict_val = evict_cursor['a']
        evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()

        # Drop the pin now that eviction has run.
        cursor.close()

        self.assertEqual(evict_val, 'c')
