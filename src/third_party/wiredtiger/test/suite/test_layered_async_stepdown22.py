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

# A tie between the constituents can appear mid-walk with no prepared conflict involved at all: a
# new transaction context can see a write that lands the alternate on the same key the current
# cursor already holds. That tie must still be broken toward ingest, and it must not cause the
# current cursor to be stepped twice in the same call once it turns out to be the stable side.
@disagg_test_class
class test_layered_async_stepdown22(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    def conn_config(self):
        return 'statistics=(all),precise_checkpoint=true,disaggregated=(role="leader",stepdown_write_mirroring=false)'

    directions = [
        ('next', dict(forward=True)),
        ('prev', dict(forward=False)),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, directions)

    test_name = __qualname__
    uri = f'layered:{test_name}'

    def test_tie_from_context_change_with_stable_current(self):
        self.set_global_ts(1, 1)
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # All three keys start out stable-only, so the walk's current cursor is stable throughout.
        self.write_at(self.uri, {'key2': 'v', 'key4': 'v', 'key6': 'v'}, 10)
        self.set_global_ts(1, 10)

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        step = cursor.next if self.forward else cursor.prev
        first_key, second_key, third_key = \
            ('key2', 'key4', 'key6') if self.forward else ('key6', 'key4', 'key2')
        self.assertEqual(step(), 0)
        self.assertEqual(cursor.get_key(), first_key)
        self.assertEqual(step(), 0)
        self.assertEqual(cursor.get_key(), second_key)

        # A write in the step-down window lands on ingest, matching the key the walk's current
        # (stable) cursor is already on. A later read context is needed to see it.
        self.set_step_down_ts(20)
        writer = self.conn.open_session()
        wcursor = writer.open_cursor(self.uri, None, None)
        writer.begin_transaction()
        wcursor[second_key] = 'v-ingest'
        writer.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
        wcursor.close()
        writer.close()

        # A new transaction context clears the iteration flags, so the next step repositions the
        # alternate (ingest) from the current (stable) key instead of trusting a cached position.
        # That reposition is what produces the tie, with stable already the current cursor.
        self.session.commit_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(30))

        # The tie on the second key must not be returned twice, and the walk must still reach the
        # third key next rather than stopping or repeating: a double-step on the stable cursor here
        # would skip it.
        self.assertEqual(step(), 0)
        self.assertEqual(cursor.get_key(), third_key)

        self.session.commit_transaction()
        cursor.close()

if __name__ == '__main__':
    wttest.run()
