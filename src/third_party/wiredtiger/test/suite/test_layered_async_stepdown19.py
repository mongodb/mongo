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
from wtscenario import make_scenarios

# A walk returns every available key before a prepared key, then blocks. Reversing returns the last
# key again, followed by all earlier keys, whichever constituent holds the prepared update.
@disagg_test_class
class test_layered_async_stepdown19(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),precise_checkpoint=true,' \
        'debug_mode=(disagg_stepdown_prepare=true),'

    # Which constituent holds the prepared update, and therefore which one blocks.
    blocked_targets = [
        ('stable_blocked', dict(prepare_on_stable=True)),
        ('ingest_blocked', dict(prepare_on_stable=False)),
    ]

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader",stepdown_write_mirroring=false)'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, blocked_targets)

    test_name = __qualname__
    uri = f'layered:{test_name}'

    def write_at(self, session, cursor, items, commit_ts):
        session.begin_transaction()
        for k, v in items.items():
            cursor[k] = v
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def test_reverse_while_blocked(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
                                 ',stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Interleave the keys and place another available key close to the prepared key.
        stable = {f'key{i:04d}': 'v' for i in range(0, 40, 2)}
        ingest = {f'key{i:04d}': 'v' for i in range(1, 40, 2)}
        if self.prepare_on_stable:
            prepared_key, extra_key = 'key0020', 'key0018a'
            del stable[prepared_key]
            ingest[extra_key] = 'v'
        else:
            prepared_key, extra_key = 'key0019', 'key0017a'
            del ingest[prepared_key]
            stable[extra_key] = 'v'

        cursor = self.session.open_cursor(self.uri, None, None)
        self.write_at(self.session, cursor, stable, 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        cursor.close()

        prepare_session = self.conn.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri, None, None)

        # A prepare on stable must begin before the step-down timestamp is announced; one on ingest
        # must begin after it.
        if self.prepare_on_stable:
            prepare_session.begin_transaction()
            prepare_cursor[prepared_key] = 'p'
            prepare_session.prepare_transaction(
                'prepare_timestamp=' + self.timestamp_str(12))

        self.conn.set_timestamp('step_down_timestamp=' + self.timestamp_str(15))
        writer = self.conn.open_session()
        wcursor = writer.open_cursor(self.uri, None, None)
        self.write_at(writer, wcursor, ingest, 20)
        wcursor.close()
        writer.close()

        if not self.prepare_on_stable:
            prepare_session.begin_transaction()
            prepare_cursor[prepared_key] = 'p'
            prepare_session.prepare_transaction(
                'prepare_timestamp=' + self.timestamp_str(30))

        all_keys = sorted(set(stable) | set(ingest))
        before_block = [key for key in all_keys if key < prepared_key]

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(35))

        # Walk forward until the prepared key blocks the walk. The walk must genuinely block: a
        # walk that runs off the end without ever hitting the conflict proves nothing.
        forward = []
        blocked = False
        while True:
            try:
                if cursor.next() != 0:
                    break
            except wiredtiger.WiredTigerError as e:
                self.assertIn(
                    wiredtiger.wiredtiger_strerror(wiredtiger.WT_PREPARE_CONFLICT), str(e))
                blocked = True
                break
            forward.append(cursor.get_key())
        self.assertTrue(blocked, 'the prepared update never blocked the forward walk')
        self.assertEqual(forward, before_block)

        # Reverse while still blocked. As with a btree cursor, the walk returns the last key it
        # returned again, then everything before it in reverse.
        backward = []
        while cursor.prev() == 0:
            backward.append(cursor.get_key())

        self.session.rollback_transaction()
        cursor.close()
        prepare_session.rollback_transaction()
        prepare_cursor.close()
        prepare_session.close()

        expected = list(reversed(before_block))
        self.assertEqual(backward, expected)

if __name__ == '__main__':
    wttest.run()
