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

import wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# Test we don't revome the user tombstones from the ingest table until they are included in a checkpoint.

@disagg_test_class
class test_layered49(wttest.WiredTigerTestCase, DisaggConfigMixin):
    disagg_storages = gen_disagg_storages('test_layered49', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = 'disaggregated=(page_log=palm),cache_size=10MB,'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    nitems = 100
    timestamp = 1

    session_follow = None
    conn_follow = None

    def leader_put_data(self, uri, value_prefix = '', low = 1, high = nitems):
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(low, high):
            self.session.begin_transaction()
            cursor[str(i)] = value_prefix + str(i)
            self.timestamp += 1
            ts_cfg = "commit_timestamp=" + self.timestamp_str(self.timestamp)
            self.session.commit_transaction(ts_cfg)
        cursor.close()

    def checkpoint(self):
        self.timestamp += 1
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.timestamp))
        self.session.checkpoint()

    def create_follower(self):
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                                self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session('')

    def test_remove(self):
        uri = "layered:test_layered49"
        # Setup
        self.session.create(uri, 'key_format=S,value_format=S')
        self.create_follower()

        # Insert some data and checkpoint
        self.leader_put_data(uri)
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        cursor = self.session_follow.open_cursor(uri, None, None)

        # Delete all the data
        for i in range(1, self.nitems):
            self.session_follow.begin_transaction()
            cursor.set_key(str(i))
            cursor.remove()
            self.timestamp += 1
            self.session_follow.commit_transaction("commit_timestamp=" + self.timestamp_str(self.timestamp))

        # Make the delete globally visible
        self.conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(self.timestamp) + ',oldest_timestamp=' + self.timestamp_str(self.timestamp))

        # Force the page to be evicted
        session_evict = self.conn_follow.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            evict_cursor.set_key(str(i))
            self.assertEqual(evict_cursor.search(), wiredtiger.WT_NOTFOUND)
            evict_cursor.reset()
        evict_cursor.close()

        # Verify that the data is still not visible
        self.session_follow.begin_transaction("read_timestamp=" + self.timestamp_str(self.timestamp))
        for i in range(1, self.nitems):
            cursor.set_key(str(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session_follow.rollback_transaction()

    def test_truncate(self):
        uri = "layered:test_layered49"
        # Setup
        self.session.create(uri, 'key_format=S,value_format=S')
        self.create_follower()

        # Insert some data and checkpoint
        self.leader_put_data(uri)
        self.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

        cursor = self.session_follow.open_cursor(uri, None, None)
        cursor.next()

        # Truncate all the data
        self.session_follow.begin_transaction()
        self.session_follow.truncate(None, cursor, None, None)
        self.timestamp = self.timestamp + 1
        self.session_follow.commit_transaction("commit_timestamp=" + self.timestamp_str(self.timestamp))

        # Make the delete globally visible
        self.conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(self.timestamp) + ',oldest_timestamp=' + self.timestamp_str(self.timestamp))

        # Force the page to be evicted
        session_evict = self.conn_follow.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            evict_cursor.set_key(str(i))
            self.assertEqual(evict_cursor.search(), wiredtiger.WT_NOTFOUND)
            evict_cursor.reset()
        evict_cursor.close()

        # Verify that the data is still not visible
        self.session_follow.begin_transaction("read_timestamp=" + self.timestamp_str(self.timestamp))
        for i in range(1, self.nitems):
            cursor.set_key(str(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session_follow.rollback_transaction()
