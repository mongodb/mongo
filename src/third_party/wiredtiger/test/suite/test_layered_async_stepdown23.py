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

# A forward scan blocks first on ingest, then on stable. Roll back each prepared transaction when
# its conflict is reported and verify that the scan returns the remaining keys and values in order.
@disagg_test_class
class test_layered_async_stepdown23(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),precise_checkpoint=true,' \
        'debug_mode=(disagg_stepdown_prepare=true),'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader",stepdown_write_mirroring=false)'

    scenarios = gen_disagg_storages(disagg_only=True)

    test_name = __qualname__
    uri = f'layered:{test_name}'

    def test_two_independent_blocks_on_opposite_constituents(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Prepare key3 before announcing the step-down, between the committed keys key1 and key5.
        self.write_at(self.uri, {'key1': 'v', 'key5': 'v'}, 10)
        self.set_global_ts(1, 10)

        stable_prep_session = self.conn.open_session()
        stable_prep_cursor = stable_prep_session.open_cursor(self.uri, None, None)
        stable_prep_session.begin_transaction()
        stable_prep_cursor['key3'] = 'v'
        stable_prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(12))

        # The walk returns key2a before blocking on key2b, then blocks on key3 after key2b resolves.
        self.set_step_down_ts(15)
        self.write_at(self.uri, {'key2a': 'v'}, 18)

        ingest_prep_session = self.conn.open_session()
        ingest_prep_cursor = ingest_prep_session.open_cursor(self.uri, None, None)
        ingest_prep_session.begin_transaction()
        ingest_prep_cursor['key2b'] = 'v'
        ingest_prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(22))

        self.write_at(self.uri, {'key4': 'v'}, 25)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))

        seen = []
        conflicts = 0
        stable_resolved = ingest_resolved = False

        while True:
            try:
                if cursor.next() != 0:
                    break
            except wiredtiger.WiredTigerError as e:
                self.assertIn(
                    wiredtiger.wiredtiger_strerror(wiredtiger.WT_PREPARE_CONFLICT), str(e))
                conflicts += 1
                self.assertLess(conflicts, 10, 'the walk never resumed past a blocking key')
                # Resolve the prepared keys in the order the walk reaches them.
                if not ingest_resolved:
                    ingest_prep_session.rollback_transaction()
                    ingest_prep_cursor.close()
                    ingest_prep_session.close()
                    ingest_resolved = True
                elif not stable_resolved:
                    stable_prep_session.rollback_transaction()
                    stable_prep_cursor.close()
                    stable_prep_session.close()
                    stable_resolved = True
                continue
            seen.append((cursor.get_key(), cursor.get_value()))

        self.session.rollback_transaction()
        cursor.close()
        if not stable_resolved:
            stable_prep_session.rollback_transaction()
            stable_prep_cursor.close()
            stable_prep_session.close()
        if not ingest_resolved:
            ingest_prep_session.rollback_transaction()
            ingest_prep_cursor.close()
            ingest_prep_session.close()

        # Both prepared transactions are rolled back, so neither key3 nor key2b ever existed as a
        # visible value: every other key is returned exactly once, in order.
        self.assertEqual(conflicts, 2, 'expected exactly one block per constituent')
        self.assertEqual(seen, [(key, 'v') for key in ('key1', 'key2a', 'key4', 'key5')])

if __name__ == '__main__':
    wttest.run()
