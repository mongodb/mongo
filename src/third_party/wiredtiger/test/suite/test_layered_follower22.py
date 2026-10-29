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

# test_layered_follower22.py
#    Read without a read timestamp on a node that has just stepped down, before any checkpoint has
# been delivered to it. The node's newest checkpoint is the one it wrote as the leader, so a snapshot
# established here must cover it: nothing else has told this node about that checkpoint.
@disagg_test_class
class test_layered_follower22(wttest.WiredTigerTestCase):
    test_name = __qualname__

    uri = f'layered:{test_name}'
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def test_read_after_step_down_before_any_delivery(self):
        session = self.conn.open_session('')
        session.create(self.uri, self.table_config)

        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        cursor['key'] = 'value'
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()

        # Seal it into a checkpoint of this node's own making.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        session.checkpoint()

        # Step down. No checkpoint has been delivered, so the newest checkpoint this node knows of
        # is still its own.
        self.conn.set_timestamp('step_down_timestamp=' + self.timestamp_str(10))
        self.conn.reconfigure('disaggregated=(role="follower")')

        # A snapshot established now pins the node's newest checkpoint, so binding the stable
        # constituent is consistent with it and the read is served.
        session.begin_transaction()
        cursor = session.open_cursor(self.uri)
        cursor.set_key('key')
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 'value')
        cursor.close()
        session.rollback_transaction()
        session.close()
