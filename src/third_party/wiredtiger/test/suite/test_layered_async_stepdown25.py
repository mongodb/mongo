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


# Verify that a scan resumes with exact keys and values after a prepared update rolls back,
# including step-down while blocked, both scan directions, and write mirroring on or off.
@disagg_test_class
class test_layered_async_stepdown25(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    scenarios = make_scenarios(gen_disagg_storages(disagg_only=True), [
        ('next', dict(forward=True)), ('prev', dict(forward=False))], [
        ('unmirrored', dict(write_mirroring=False)), ('mirrored', dict(write_mirroring=True))], [
        ('no_step_down', dict(step_down=False)), ('step_down', dict(step_down=True))])

    def conn_config(self):
        return 'statistics=(all),precise_checkpoint=true,' \
            'debug_mode=(disagg_stepdown_prepare=true),' \
            'disaggregated=(role="leader",stepdown_write_mirroring=' + \
            str(self.write_mirroring).lower() + ')'

    def key(self, rank):
        return f'key{rank if self.forward else 5 - rank}'

    def test_blocked_walk_with_step_down_writes(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.write_at(self.uri, {self.key(rank): f'v{rank}' for rank in (1, 3)}, 10)
        self.set_global_ts(1, 10)

        prepared = self.conn.open_session()
        prepare_cursor = prepared.open_cursor(self.uri)
        prepared.begin_transaction()
        prepare_cursor[self.key(3)] = 'prepared'
        prepared.prepare_transaction('prepare_timestamp=' + self.timestamp_str(12))

        self.set_step_down_ts(15)
        self.write_at(self.uri, {self.key(rank): f'v{rank}' for rank in (2, 4)}, 20)
        cursor = self.session.open_cursor(self.uri)
        step = cursor.next if self.forward else cursor.prev
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        actual = []
        for rank in (1, 2):
            self.assertEqual(step(), 0)
            actual.append((cursor.get_key(), cursor.get_value()))
            self.assertEqual(actual[-1], (self.key(rank), f'v{rank}'))
        for _ in range(2):
            self.assertRaisesException(wiredtiger.WiredTigerError, step, '/WT_PREPARE_CONFLICT/')

        if self.step_down:
            self.complete_step_down(15)
        try:
            prepared.rollback_transaction()
        except wiredtiger.WiredTigerError:
            self.assertTrue(self.step_down)
            self.assertEqual(prepared.get_last_error()[1], wiredtiger.WT_CONFLICT_DISAGG)
        prepare_cursor.close()
        prepared.close()

        for rank in (3, 4):
            self.assertEqual(step(), 0)
            actual.append((cursor.get_key(), cursor.get_value()))
            self.assertEqual(actual[-1], (self.key(rank), f'v{rank}'))
        self.assertEqual(step(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(actual, [(self.key(rank), f'v{rank}') for rank in (1, 2, 3, 4)])
        self.session.rollback_transaction()
        cursor.close()
        # Leave the step-down window only after the control scan has completed.
        if not self.step_down:
            self.complete_step_down(15)
