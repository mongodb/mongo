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

# A walk blocked on a stable prepared update, whose node then steps down, resumes from the key it
# stopped at. The stable cursor is reopened on the follower's checkpoint like any other, which holds
# no prepared update, so the walk returns the stopped key with its value from before the update.
@disagg_test_class
class test_layered_async_stepdown17(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    def conn_config(self):
        return 'statistics=(all),precise_checkpoint=true,' \
            'debug_mode=(disagg_stepdown_prepare=true),' \
            'disaggregated=(role="leader",stepdown_write_mirroring=' + \
            str(self.write_mirroring).lower() + ')'

    directions = [
        ('next', dict(forward=True)),
        ('prev', dict(forward=False)),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    # Vary the mirroring configuration; all writes precede the step-down window.
    mirroring_modes = [
        ('unmirrored', dict(write_mirroring=False)),
        ('mirrored', dict(write_mirroring=True)),
    ]
    scenarios = make_scenarios(disagg_storages, directions, mirroring_modes)

    test_name = __qualname__
    uri = f'layered:{test_name}'

    def test_walk_blocked_across_step_down(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'key10': 'v', 'key20': 'v', 'key30': 'v'}, 10)
        self.set_global_ts(1, 10)

        prepare_session = self.conn.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri, None, None)
        prepare_session.begin_transaction()
        prepare_cursor['key20'] = 'v-prepared'
        prepare_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(12))

        # The reader begins before the step-down is announced, so it reads stable alone.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(14))
        step = cursor.next if self.forward else cursor.prev
        self.assertEqual(step(), 0)
        self.assertEqual(cursor.get_key(), 'key10' if self.forward else 'key30')
        self.assertRaisesException(wiredtiger.WiredTigerError, step,
            '/conflict with a prepared update/')

        self.set_step_down_ts(15)
        self.complete_step_down(15)

        restore_before = self.get_stat(wiredtiger.stat.conn.layered_curs_restore_walk)
        resumed = []
        while step() == 0:
            resumed.append((cursor.get_key(), cursor.get_value()))
        self.assertEqual(resumed,
            [('key20', 'v'), ('key30', 'v')] if self.forward else [('key20', 'v'), ('key10', 'v')])
        restore_after = self.get_stat(wiredtiger.stat.conn.layered_curs_restore_walk)

        # Resetting releases the position, which pins the live stable table. Resolving a straddling
        # prepared transaction on the follower is not supported yet (FIXME-WT-18422), so the
        # rollback can be refused.
        cursor.reset()
        self.session.rollback_transaction()
        cursor.close()
        try:
            prepare_session.rollback_transaction()
        except wiredtiger.WiredTigerError:
            self.assertEqual(prepare_session.get_last_error()[1], wiredtiger.WT_CONFLICT_DISAGG)
        prepare_cursor.close()
        prepare_session.close()
        self.assertEqual(restore_after, restore_before + 1)

    # The walk blocks on a prepared update, then a reversal hits a prepare conflict on the key it
    # last returned, then the step-down completes. The walk then reverses on the follower, which
    # returns the last returned key again, then everything before it.
    def test_walk_reversal_conflict_across_step_down(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {'key2': 'v', 'key4': 'v', 'key6': 'v', 'key8': 'v'}, 10)
        self.set_global_ts(1, 10)

        first = self.conn.open_session()
        first_cursor = first.open_cursor(self.uri, None, None)
        first.begin_transaction()
        first_cursor['key6' if self.forward else 'key4'] = 'v-prepared'
        first.prepare_transaction('prepare_timestamp=' + self.timestamp_str(12))

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(14))
        step, back = (cursor.next, cursor.prev) if self.forward else (cursor.prev, cursor.next)
        for key in ('key2', 'key4') if self.forward else ('key8', 'key6'):
            self.assertEqual(step(), 0)
            self.assertEqual(cursor.get_key(), key)
        self.assertRaisesException(wiredtiger.WiredTigerError, step,
            '/conflict with a prepared update/')

        # The key the walk last returned is prepared by another transaction, and read under a new
        # snapshot, so reversing conflicts on it too.
        second = self.conn.open_session()
        second_cursor = second.open_cursor(self.uri, None, None)
        second.begin_transaction()
        second_cursor['key4' if self.forward else 'key6'] = 'v-prepared'
        second.prepare_transaction('prepare_timestamp=' + self.timestamp_str(30))
        self.session.commit_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(35))
        self.assertRaisesException(wiredtiger.WiredTigerError, back,
            '/conflict with a prepared update/')
        second.rollback_transaction()
        second_cursor.close()
        second.close()

        self.set_step_down_ts(32)
        self.complete_step_down(32)

        returned = []
        while back() == 0:
            returned.append(cursor.get_key())
        self.assertEqual(returned, ['key4', 'key2'] if self.forward else ['key6', 'key8'])

        # Resetting releases the position, which pins the live stable table. Resolving a straddling
        # prepared transaction on the follower is not supported yet (FIXME-WT-18422), so the
        # rollback can be refused.
        cursor.reset()
        self.session.rollback_transaction()
        cursor.close()
        try:
            first.rollback_transaction()
        except wiredtiger.WiredTigerError:
            self.assertEqual(first.get_last_error()[1], wiredtiger.WT_CONFLICT_DISAGG)
        first_cursor.close()
        first.close()

if __name__ == '__main__':
    wttest.run()
