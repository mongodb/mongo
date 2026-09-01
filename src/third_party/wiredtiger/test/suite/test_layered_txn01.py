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

# test_layered_txn01.py
#   Durability of an ordinary committed transaction that spans several layered tables. Each table
#   is a separate pair of btrees, so a transaction covering all of them must survive a restart
#   all-or-nothing: after picking the checkpoint back up, either every table holds the write or
#   none does. Whether it survives at all depends on the scenario, which either does or does not
#   advance the stable timestamp past the commit before checkpointing.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_txn01(wttest.WiredTigerTestCase):
    test_name = __qualname__

    # Whether the stable timestamp is advanced past the cross-table commit before the checkpoint
    # that the restart picks up.
    durability = [
        ('stable_advanced', dict(durable=True)),
        ('stable_behind', dict(durable=False)),
    ]
    table_counts = [
        ('two_tables', dict(ntables=2)),
        ('three_tables', dict(ntables=3)),
    ]
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, durability, table_counts)

    def conn_config(self):
        return self.extensionsConfig() + ',create,statistics=(all),precise_checkpoint=true,' \
            + 'disaggregated=(role="leader")'

    def uris(self):
        return [f'layered:{self.test_name}_{i}' for i in range(self.ntables)]

    # Commit one transaction that writes the key to every table.
    def put_all(self, key, value, ts):
        cursors = [self.session.open_cursor(uri) for uri in self.uris()]
        self.session.begin_transaction()
        for cursor in cursors:
            cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        for cursor in cursors:
            cursor.close()

    # The value each table holds for the key, or None where it is absent.
    def read_all(self, key):
        values = []
        for uri in self.uris():
            cursor = self.session.open_cursor(uri)
            cursor.set_key(key)
            values.append(cursor.get_value() if cursor.search() == 0 else None)
            cursor.close()
        return values

    def test_cross_table_commit_survives_restart(self):
        for uri in self.uris():
            self.session.create(uri, 'key_format=S,value_format=S')

        # A baseline that is stable and checkpointed before the transaction under test, so the
        # restart has something to come back to in every scenario.
        self.put_all('baseline', 'v0', 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.put_all('spanning', 'v1', 20)
        if self.durable:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

        self.restart_without_local_files(step_up=True)

        # The restart resets the stable timestamp, which a precise checkpoint requires.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        self.assertEqual(self.read_all('baseline'), ['v0'] * self.ntables)

        # The point of the test: the tables must agree with each other, whichever way it went.
        expected = 'v1' if self.durable else None
        self.assertEqual(self.read_all('spanning'), [expected] * self.ntables)

if __name__ == '__main__':
    wttest.run()
