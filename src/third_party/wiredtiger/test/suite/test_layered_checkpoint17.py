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

# A node that checkpoints a table while it is still empty records a checkpoint for it without
# writing any pages. Hand leadership to a second node, have it write the table's first real
# checkpoint, and hand leadership back. Every node has to see the data throughout: a node that
# treats the new checkpoint as one it already has drops the data while believing it holds it, and
# leading again then makes the loss permanent.

@disagg_test_class
class test_layered_checkpoint17(wttest.WiredTigerTestCase):
    test_name = __qualname__
    num_items = 100

    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, [
        ('layered-prefix', dict(prefix='layered:', table_config='')),
        ('shared', dict(prefix='table:',
                        table_config=',block_manager=disagg,log=(enabled=false)')),
    ])

    def insert_data(self, session, uri, value_prefix, ts):
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        for i in range(self.num_items):
            cursor[str(i)] = value_prefix + str(i)
        cursor.close()
        session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')

    def check_data(self, session, uri, value_prefix):
        cursor = session.open_cursor(uri)
        for i in range(self.num_items):
            self.assertEqual(cursor[str(i)], value_prefix + str(i))
        cursor.close()

    def test_layered_checkpoint17(self):
        uri = self.prefix + self.test_name

        # Node 2 starts out following node 1.
        conn2 = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_base_config +
            'disaggregated=(role="follower")')
        session2 = conn2.open_session('')

        # Node 1 creates the table and checkpoints it while it is still empty, so the checkpoint it
        # records for the table has no address behind it.
        self.session.create(uri, 'key_format=S,value_format=S' + self.table_config)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.session.checkpoint()

        # Node 2 picks up that checkpoint, then node 1 hands over to it.
        self.disagg_advance_checkpoint_and_wait(conn2)
        self.conn.reconfigure('disaggregated=(role="follower")')
        conn2.reconfigure('disaggregated=(role="leader")')

        # Node 2 writes the table's first real checkpoint.
        self.insert_data(session2, uri, 'v1-', 20)
        conn2.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')
        session2.checkpoint()

        # Node 1 picks up node 2's checkpoint and must see the data it carries.
        self.disagg_advance_checkpoint_and_wait(self.conn, conn2)
        self.check_data(self.session, uri, 'v1-')

        # The same has to hold once node 1 leads again: an ignored checkpoint would make the loss
        # permanent by checkpointing the table without node 2's data.
        conn2.reconfigure('disaggregated=(role="follower")')
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.check_data(self.session, uri, 'v1-')

        self.insert_data(self.session, uri, 'v2-', 30)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(30)}')
        self.session.checkpoint()

        self.disagg_advance_checkpoint_and_wait(conn2)
        self.check_data(session2, uri, 'v2-')

        session2.close()
        conn2.close()
