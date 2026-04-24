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
#
# test_prepare_discover08.py
#   After reopening the connection, "prepared_discover:" must discover and
#   allow claims of the persisted prepared transaction even when no cursor
#   has yet been opened on the layered table on the new connection. Exercised
#   for both roles the connection can come back as.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_prepare_discover08(wttest.WiredTigerTestCase):
    tablename = 'test_prepare_discover08'
    uri = 'layered:' + tablename

    role_scenarios = [
        ('standby', dict(reopen_role='follower')),
        ('primary', dict(reopen_role='leader')),
    ]
    disagg_storages = gen_disagg_storages('test_prepare_discover08', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, role_scenarios)

    conn_base_config = ('cache_size=10MB,statistics=(all),'
                        'precise_checkpoint=true,preserve_prepared=true,')

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    def test_prepare_discover_first_cursor_on_connection(self):
        prepared_id = 123

        # Leader: commit some data, prepare a later transaction, advance stable
        # past the prepare, then checkpoint so the prepared update is included.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        for i in range(1, 4):
            cursor[i] = f'committed_value_{i}'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        self.session.begin_transaction()
        for i in range(4, 7):
            cursor[i] = f'prepared_value_{i}'
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id=' + self.prepared_id_str(prepared_id))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))

        ckpt_session = self.conn.open_session()
        ckpt_session.checkpoint()
        ckpt_session.close()

        cursor.close()

        # Capture checkpoint metadata while the original leader connection is
        # still open, then reopen in the role this scenario targets.
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        if self.reopen_role == 'follower':
            reopen_config = (self.conn_base_config +
                             'disaggregated=(role="follower",'
                             f'checkpoint_meta="{checkpoint_meta}")')
        else:
            reopen_config = self.conn_base_config + 'disaggregated=(role="leader")'
        self.reopen_conn(config=reopen_config)

        # Opening "prepared_discover:" must work as the first cursor on the
        # reopened connection; successful discovery must not depend on a
        # prior cursor having been opened on the layered table.
        prepared_discover_cursor = self.session.open_cursor('prepared_discover:')

        claim_session = self.conn.open_session()
        discovered = []
        while prepared_discover_cursor.next() == 0:
            discovered_id = prepared_discover_cursor.get_key()
            discovered.append(discovered_id)
            # Every prepared transaction found by the cursor must be claimed
            # before the cursor is closed; claim it and commit here.
            claim_session.begin_transaction(
                'claim_prepared_id=' + self.prepared_id_str(discovered_id))
            claim_session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(200) +
                ',durable_timestamp=' + self.timestamp_str(210))

        self.assertEqual(discovered, [prepared_id])
        prepared_discover_cursor.close()
        claim_session.close()
