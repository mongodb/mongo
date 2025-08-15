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

import platform, wttest, wiredtiger
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from test_layered23 import Oplog
from wtscenario import make_scenarios

# test_layered38.py
# Test garbage collecting redundant content in the ingest table
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered38(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = ',create,cache_size=10GB,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                 + 'disaggregated=(page_log=palm,lose_all_my_data=true),precise_checkpoint=true,'

    disagg_storages = gen_disagg_storages('test_layered38', disagg_only = True)

    scenarios = make_scenarios(disagg_storages)

    uri = 'layered:test_layered38'
    ingest_uri = 'file:test_layered38.wt_ingest'

    nitems = 1000

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    # Use a debug cursor to reconcile/evict the ingest table.
    # This will GC content when possible.
    def evict_ingest(self, session, ts):
        # Trigger eviction on the ingest table
        evict_cursor = session.open_cursor("file:test_layered38.wt_ingest", None, "debug=(release_evict)")
        for i in range(1, self.nitems):
            session.begin_transaction(f'read_timestamp={self.timestamp_str(ts)}')
            evict_cursor.set_key(str(i))
            ret = evict_cursor.search()
            self.assertTrue(ret == 0 or ret == wiredtiger.WT_NOTFOUND) # It might not find it
            evict_cursor.reset()
            session.rollback_transaction()
        evict_cursor.close()

    # Return the number of items in the ingest table.
    def count_ingest(self, session, ts=None):
        if ts != None:
            session.begin_transaction(f'read_timestamp={self.timestamp_str(ts)}')
        cursor = session.open_cursor(self.ingest_uri)
        count = 0
        for k,v in cursor:
            count += 1
        cursor.close()
        if ts != None:
            session.rollback_transaction()
        return count

    def test_gc_ingest_table(self):
        # Create the oplog
        oplog = Oplog(value_size=500)

        # Create the table on leader and tell oplog about it
        self.session.create(self.uri, "allocation_size=512,leaf_page_max=512,key_format=S,value_format=S")
        t = oplog.add_uri(self.uri)

        # Create the follower and create its table
        # To keep this test relatively easy, we're only using a single URI.
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, "allocation_size=512,leaf_page_max=512,key_format=S,value_format=S")

        # Load the data for oplog
        oplog.insert(t, self.nitems)

        # Apply it to the leader WT.
        oplog.apply(self, self.session, 0, self.nitems)
        oplog.check(self, self.session, 0, self.nitems)

        ts = oplog.last_timestamp()
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts)}')

        # On the follower -
        # Apply all the entries to follower
        oplog.apply(self, session_follow, 0, self.nitems)
        oplog.check(self, session_follow, 0, self.nitems)

        # Ensure everything is in the ingest table
        count = self.count_ingest(session_follow)
        self.assertEqual(count, self.nitems)

        # Hold a cursor open on the layered table, and on the ingest as well.
        session_follow2 = conn_follow.open_session('')
        hold_cursor = session_follow2.open_cursor(self.uri)
        hold_cursor.next()

        # Take a checkpoint and advance it, make sure everything is still good
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)
        oplog.check(self, session_follow, 0, self.nitems)

        # At this point, everything in the ingest table is redundant, as it's
        # also in the stable table on the follower. However, it cannot be removed
        # as there is a cursor open.
        self.evict_ingest(session_follow, ts)
        count = self.count_ingest(session_follow)
        self.assertEqual(count, self.nitems)

        # Close the cursor held open.
        hold_cursor.close()

        # Now eviction should remove all the items from the ingest table.
        self.evict_ingest(session_follow, ts)
        count = self.count_ingest(session_follow)
        self.assertEqual(count, 0)

    def test_gc_ingest_table_with_remove(self):
        if platform.processor() == 's390x':
            self.skipTest("FIXME-WT-15000: not working on zSeries")

        # Create the oplog
        oplog = Oplog(value_size=500)

        # Create the table on leader and tell oplog about it
        self.session.create(self.uri, "allocation_size=512,leaf_page_max=512,key_format=S,value_format=S")
        t = oplog.add_uri(self.uri)

        # Create the follower and create its table
        # To keep this test relatively easy, we're only using a single URI.
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, "allocation_size=512,leaf_page_max=512,key_format=S,value_format=S")

        # Load the data for oplog
        oplog.insert(t, self.nitems)

        ts = oplog.last_timestamp()

        # Remove the data
        oplog.remove(t, self.nitems)

        # Apply the insert to the leader WT.
        oplog.apply(self, self.session, 0, self.nitems)

        # Apply the delete to the leader WT after checkpoint.
        oplog.apply(self, self.session, self.nitems, self.nitems)
        oplog.check(self, self.session, 0, 2 * self.nitems)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts)}')

        # On the follower -
        # Apply all the entries to follower
        oplog.apply(self, session_follow, 0, 2 * self.nitems)
        oplog.check(self, session_follow, 0, 2 * self.nitems)

        # Ensure everything is in the ingest table
        count = self.count_ingest(session_follow)
        self.assertEqual(count, self.nitems)

        # Hold a cursor open on the layered table, and on the ingest as well.
        session_follow2 = conn_follow.open_session('')
        hold_cursor = session_follow2.open_cursor(self.uri)
        hold_cursor.next()

        # Take a checkpoint and advance it, make sure everything is still good
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)
        oplog.check(self, session_follow, 0, 2 * self.nitems)

        # At this point, the inserts in the ingest table are redundant but the delets are not.
        self.evict_ingest(session_follow, ts)
        count = self.count_ingest(session_follow, ts)
        self.assertEqual(count, self.nitems)

        # Close the cursor held open.
        hold_cursor.close()

        # Eviction still cannot remove all the records from the ingest table because the deletes
        # are not in the stable table.
        self.evict_ingest(session_follow, ts)
        count = self.count_ingest(session_follow, ts)
        self.assertEqual(count, self.nitems)

        # Hold a cursor open on the layered table, and on the ingest as well.
        hold_cursor = session_follow2.open_cursor(self.uri)
        hold_cursor.next()

        new_ts = oplog.last_timestamp()
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(new_ts)}')

        # Take a new checkpoint and advance it, make sure everything is still good
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)
        oplog.check(self, session_follow, 0, 2 * self.nitems)

        # At this point, everything in the ingest table is redundant, as it's
        # also in the stable table on the follower. However, it cannot be removed
        # as there is a cursor open.
        self.evict_ingest(session_follow, ts)
        count = self.count_ingest(session_follow, ts)
        self.assertEqual(count, self.nitems)

        # Close the cursor held open.
        hold_cursor.close()

        # Trigger advance checkpoint code again to set the prune timestamp to the last
        # checkpoint timestamp. We couldn't do that because there was a cursor holding the old content.
        self.disagg_advance_checkpoint(conn_follow)

        # Now eviction should remove all the items from the ingest table.
        self.evict_ingest(session_follow, ts)
        count = self.count_ingest(session_follow, ts)
        self.assertEqual(count, 0)
