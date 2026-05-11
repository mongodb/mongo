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

# test_prepare_discover09.py
#   A single prepared transaction that touches both a layered table and a
#   regular (non-layered) table must be discoverable via
#   "prepared_discover:" on the follower. Resolving the claim (commit or
#   rollback) must apply to the updates on every table the transaction
#   wrote, not only the layered ones.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@wttest.skip_for_hook("tiered", "Layered tables are not supported with tiered storage")
@disagg_test_class
class test_prepare_discover09(wttest.WiredTigerTestCase):
    conn_base_config = ('cache_size=10MB,statistics=(all),'
                        'precise_checkpoint=true,preserve_prepared=true,'
                        'log=(enabled=false),')

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    resolution_scenarios = [
        ('commit', dict(resolve='commit')),
        ('rollback', dict(resolve='rollback')),
    ]
    disagg_storages = gen_disagg_storages('test_prepare_discover09', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, resolution_scenarios)

    local_uri = 'table:local_plain'
    layered_uri = 'layered:layered_tbl'
    # Disjoint ranges let assert_table_state distinguish "always visible"
    # from "visible only if the prepared transaction committed".
    committed_keys = range(1, 4)
    prepared_keys = range(4, 7)

    def committed_value(self, i):
        return f'committed_{i}'

    def prepared_value(self, i):
        return f'prepared_{i}'

    def populate_and_prepare(self, prepared_id):
        """
        Commit data to both tables, then prepare a single transaction
        whose updates span both tables. Advance stable past the prepare
        and checkpoint so the prepared transaction survives a reopen.
        """
        self.session.create(self.local_uri,
            'key_format=i,value_format=S,log=(enabled=false)')
        self.session.create(self.layered_uri, 'key_format=i,value_format=S')

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        c_local = self.session.open_cursor(self.local_uri)
        c_layered = self.session.open_cursor(self.layered_uri)

        self.session.begin_transaction()
        for i in self.committed_keys:
            c_local[i] = self.committed_value(i)
            c_layered[i] = self.committed_value(i)
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        self.session.begin_transaction()
        for i in self.prepared_keys:
            c_local[i] = self.prepared_value(i)
            c_layered[i] = self.prepared_value(i)
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id=' + self.prepared_id_str(prepared_id))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))

        ckpt_session = self.conn.open_session()
        ckpt_session.checkpoint()
        ckpt_session.close()

        c_local.close()
        c_layered.close()

    def reopen_as_follower(self):
        meta = self.disagg_get_complete_checkpoint_meta()
        reopen_config = (self.conn_base_config +
                         'disaggregated=(role="follower",' +
                         f'checkpoint_meta="{meta}")')
        self.reopen_conn(config=reopen_config)

    def discover_and_resolve(self, resolve):
        """
        Iterate "prepared_discover:", claiming each id and resolving it
        according to `resolve` ("commit" or "rollback"). Return the list
        of discovered ids.
        """
        cursor = self.session.open_cursor('prepared_discover:')
        claim_session = self.conn.open_session()
        discovered = []
        while cursor.next() == 0:
            discovered_id = cursor.get_key()
            discovered.append(discovered_id)
            claim_session.begin_transaction(
                'claim_prepared_id=' + self.prepared_id_str(discovered_id))
            if resolve == 'commit':
                claim_session.commit_transaction(
                    'commit_timestamp=' + self.timestamp_str(200) +
                    ',durable_timestamp=' + self.timestamp_str(210))
            else:
                claim_session.rollback_transaction(
                    'rollback_timestamp=' + self.timestamp_str(210))
        cursor.close()
        claim_session.close()
        return discovered

    def assert_table_state(self, uri, resolve):
        """
        Previously-committed keys are always visible. Prepared keys are
        visible iff the claim was committed, and absent iff it was
        rolled back.
        """
        read_session = self.conn.open_session()
        read_session.begin_transaction(
            'read_timestamp=' + self.timestamp_str(250))
        c = read_session.open_cursor(uri)
        for i in self.committed_keys:
            self.assertEqual(c[i], self.committed_value(i))
        for i in self.prepared_keys:
            c.set_key(i)
            if resolve == 'commit':
                self.assertEqual(c.search(), 0)
                self.assertEqual(c.get_value(), self.prepared_value(i))
            else:
                self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        c.close()
        read_session.rollback_transaction()
        read_session.close()

    def test_cross_table_prepared_transaction_is_claimable(self):
        prepared_id = 0x1234

        self.populate_and_prepare(prepared_id)
        self.reopen_as_follower()

        # A single prepared transaction produces a single prepared id,
        # regardless of how many tables it touched.
        discovered = self.discover_and_resolve(self.resolve)
        self.assertEqual(discovered, [prepared_id])

        # Resolving the claim must apply to every table the prepared
        # transaction wrote both the layered and the regular one.
        self.assert_table_state(self.layered_uri, self.resolve)
        self.assert_table_state(self.local_uri, self.resolve)
