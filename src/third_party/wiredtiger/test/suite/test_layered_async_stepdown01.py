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
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

# test_layered_async_stepdown01.py
#    Write routing and write semantics: writes route to stable before the step-down timestamp is
#    set and to ingest afterwards, and the merged view drives duplicate-key detection,
#    overwrite=false and reserve.
@disagg_test_class
class test_layered_async_stepdown01(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = f'layered:{test_name}'

    # Writes route to stable beforehand and to ingest afterwards.
    def test_write_routing_around_step_down_ts(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        before = {'before' + str(i) for i in range(5)}
        after = {'after' + str(i) for i in range(5)}

        self.write_at(self.uri, {k: 'stable' for k in before}, 10)
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 15), set(),
            'these writes must not be in the ingest table')
        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 15), before,
            'these writes must land in the stable table')
        self.assertEqual(self.read_keys_at(self.uri, 15), before)

        self.set_step_down_ts(20)

        self.write_at(self.uri, {k: 'ingest' for k in after}, 30)

        # The leader now reads ingest-first, merged over the live stable table: it sees both halves.
        self.assertEqual(self.read_keys_at(self.uri, 40), before | after)

        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), after)
        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 40), before,
            'later writes must not reach the stable table')

    # Update, modify and remove of stable keys route to ingest, like insert.
    def test_update_modify_remove_routing_after_step_down_ts(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        self.write_at(self.uri, {'k1': 'base', 'k2': 'base', 'k3': 'base'}, 10)
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 15), set(),
            'these writes must not be in the ingest table')
        self.assertEqual(self.read_keys_at(self.stable_uri(self.uri), 15), {'k1', 'k2', 'k3'},
            'these writes must land in the stable table')

        self.set_step_down_ts(20)

        cursor = self.session.open_cursor(self.uri, None, None)

        self.session.begin_transaction()
        cursor['k1'] = 'updated'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        self.remove_at(self.uri, ['k2'], 31)

        # Modify builds the new value on the stable base and writes the result to ingest.
        self.session.begin_transaction()
        cursor.set_key('k3')
        cursor.modify([wiredtiger.Modify('v', 0, 1)])  # 'base' -> 'vase'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(32))
        cursor.close()

        kv = self.read_kvs_at(self.uri, 40)
        self.assertEqual(kv.get('k1'), 'updated')
        self.assertEqual(kv.get('k3'), 'vase')
        self.assertNotIn('k2', kv)

        # All three landed in ingest, the remove as a tombstone shadowing stable.
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), {'k1', 'k2', 'k3'})
        self.assertEqual(self.read_kvs_at(self.stable_uri(self.uri), 40),
            {'k1': 'base', 'k2': 'base', 'k3': 'base'},
            'these writes must not touch the stable table')

        # The update, modify and tombstone all survive the completed step-down.
        self.complete_step_down(20)
        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'updated', 'k3': 'vase'})

    # All tables share one cutoff, so a single call routes every table's later writes to ingest.
    def test_multiple_tables_share_cutoff(self):
        uri1 = f'layered:{self.test_name}_multi1'
        uri2 = f'layered:{self.test_name}_multi2'
        self.set_global_ts(1, 1)
        self.session.create(uri1, 'key_format=S,value_format=S')
        self.session.create(uri2, 'key_format=S,value_format=S')

        self.write_at(uri1, {'a': 'stable'}, 10)
        self.write_at(uri2, {'b': 'stable'}, 10)

        self.set_step_down_ts(20)

        self.write_at(uri1, {'c': 'ingest'}, 30)
        self.write_at(uri2, {'d': 'ingest'}, 30)

        self.assertEqual(self.read_keys_at(self.ingest_uri(uri1), 40), {'c'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(uri2), 40), {'d'})
        self.assertEqual(self.read_keys_at(self.stable_uri(uri1), 40), {'a'})
        self.assertEqual(self.read_keys_at(self.stable_uri(uri2), 40), {'b'})
        self.assertEqual(self.read_kvs_at(uri1, 40), {'a': 'stable', 'c': 'ingest'})
        self.assertEqual(self.read_kvs_at(uri2, 40), {'b': 'stable', 'd': 'ingest'})

    # A non-overwrite insert of a stable key conflicts even though the write targets ingest.
    def test_duplicate_key_detection_while_step_down_ts_set(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'dup': 'stable'}, 10)

        self.set_step_down_ts(20)

        cursor = self.session.open_cursor(self.uri, None, "overwrite=false")
        self.session.begin_transaction()
        cursor.set_key('dup')
        cursor.set_value('again')
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.insert(),
            wiredtiger.wiredtiger_strerror(wiredtiger.WT_DUPLICATE_KEY))
        self.session.rollback_transaction()
        cursor.close()

        # The rejected insert left the stable value alone and nothing in ingest.
        self.assertEqual(self.read_kvs_at(self.uri, 40), {'dup': 'stable'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), set())

    # overwrite=false update and remove consult the merged view; the writes land in ingest.
    def test_overwrite_false_ops_while_step_down_ts_set(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'base', 'k2': 'base'}, 10)

        self.set_step_down_ts(20)

        cursor = self.session.open_cursor(self.uri, None, "overwrite=false")

        # Update of a stable key: found in the merged view, written to ingest.
        self.session.begin_transaction()
        cursor.set_key('k1')
        cursor.set_value('updated')
        self.assertEqual(cursor.update(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))

        # Remove of a stable key: a tombstone routed to ingest.
        self.session.begin_transaction()
        cursor.set_key('k2')
        self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(31))

        # Update and remove of a missing key fail across the merged view.
        self.session.begin_transaction()
        cursor.set_key('missing')
        cursor.set_value('v')
        self.assertEqual(cursor.update(), wiredtiger.WT_NOTFOUND)
        cursor.set_key('missing')
        self.assertEqual(cursor.remove(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()
        cursor.close()

        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'updated'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), {'k1', 'k2'})
        self.assertEqual(self.read_kvs_at(self.stable_uri(self.uri), 40),
            {'k1': 'base', 'k2': 'base'})

        # Both writes survive the completed step-down.
        self.complete_step_down(20)
        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'updated'})

    # A reserve conflicts with concurrent writers and leaves no content behind.
    def test_reserve_while_step_down_ts_set(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'k1': 'stable'}, 10)

        self.set_step_down_ts(20)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor.set_key('k1')
        self.assertEqual(cursor.reserve(), 0)

        # A concurrent writer conflicts with the reservation.
        wsession = self.conn.open_session()
        wcur = wsession.open_cursor(self.uri, None, None)
        wsession.begin_transaction()
        wcur.set_key('k1')
        wcur.set_value('other')
        self.expect_conflict_rollback(wcur.update)
        wsession.rollback_transaction()
        wcur.close()
        wsession.close()

        # The reserve-only commit leaves no content behind in either constituent.
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()
        self.assertEqual(self.read_kvs_at(self.uri, 40), {'k1': 'stable'})
        self.assertEqual(self.read_keys_at(self.ingest_uri(self.uri), 40), set())
