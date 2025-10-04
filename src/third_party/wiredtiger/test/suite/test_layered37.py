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
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from test_layered23 import Oplog
from wtscenario import make_scenarios

# test_layered37.py
# Test pinning the content in the ingest table
@disagg_test_class
class test_layered37(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = ',create,cache_size=10GB,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                 + 'disaggregated=(page_log=palm,lose_all_my_data=true),'

    disagg_storages = gen_disagg_storages('test_layered37', disagg_only = True)

    scenarios = make_scenarios(disagg_storages)

    uri = 'layered:test_layered37'

    nitems = 20000

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def test_ping_ingest_table(self):
        # Create the oplog
        oplog = Oplog()

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

        # Apply them to leader WT.
        oplog.apply(self, self.session, 0, self.nitems)
        oplog.check(self, self.session, 0, self.nitems)

        ts = oplog.last_timestamp()

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts)}')

        # On the follower -
        # Apply all the entries to follower
        oplog.apply(self, session_follow, 0, self.nitems)
        oplog.check(self, session_follow, 0, self.nitems)

        # Ping a read transaction
        session_follow2 = conn_follow.open_session('')
        session_follow2.begin_transaction(f'read_timestamp={self.timestamp_str(ts)}')
        cursor = session_follow2.open_cursor(self.uri)
        # Ping the cursor on the ingest table
        self.assertEqual(cursor.next(), 0)

        # Remove all data
        oplog.remove(t, self.nitems)

        # Apply them to leader WT and checkpoint.
        oplog.apply(self, self.session, self.nitems, self.nitems)
        oplog.check(self, self.session, self.nitems, self.nitems)

        # Make all the data obsolete
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())},oldest_timestamp={self.timestamp_str(oplog.last_timestamp())}')

        self.session.checkpoint()

        # Apply them to follower.
        oplog.apply(self, session_follow, self.nitems, self.nitems)
        oplog.check(self, session_follow, self.nitems, self.nitems)

        # Then advance the checkpoint and make sure everything is still good
        self.disagg_advance_checkpoint(conn_follow)

        # Trigger eviction on the ingest table
        evict_cursor = session_follow.open_cursor("file:test_layered37.wt_ingest", None, "debug=(release_evict)")
        for i in (1, self.nitems):
            session_follow.begin_transaction(f'read_timestamp={self.timestamp_str(ts)}')
            evict_cursor.set_key(str(i))
            self.assertEqual(evict_cursor.search(), 0)
            evict_cursor.reset()
            session_follow.rollback_transaction()

        count = 1
        while cursor.next() == 0:
            count += 1
        self.assertEqual(count, self.nitems)
