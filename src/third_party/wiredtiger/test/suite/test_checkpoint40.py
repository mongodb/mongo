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
from wtscenario import make_scenarios

# Test that rolling back a committed-but-unstable update restores the stable value
# of every key. Two checkpoints write two versions of each key, the second one
# newer than stable, and rollback_to_stable() then has to bring the first version
# back. Run under the disagg hook, this covers a leader.
class test_checkpoint40(wttest.WiredTigerTestCase):

    conn_config = 'cache_size=50MB,statistics=(all)'
    uri = 'table:test_checkpoint40'
    create_cfg = 'key_format=i,value_format=S,leaf_page_max=32KB'

    stable_value = 'a' * 78
    unstable_value = 'b' * 78

    scenarios = make_scenarios([
        ('prepare', dict(prepare=True)),
        ('no_prepare', dict(prepare=False)),
    ])

    nrows = 1000

    def test_rollback_unstable_update(self):
        self.session.create(self.uri, self.create_cfg)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        # Write the value that rollback to stable has to leave behind, and check
        # point it so that it is on disk before the second version exists.
        c = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            c[i] = self.stable_value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        c.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.session.checkpoint()

        # Overwrite every key at a timestamp beyond stable, so the update is
        # committed but rollback to stable must discard it.
        self.session.begin_transaction()
        c = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            c[i] = self.unstable_value
        c.close()
        if self.prepare:
            self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(3))
            self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(3))
            self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(5))
            self.session.commit_transaction()
        else:
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        # Checkpoint the unstable value onto the page.
        self.session.checkpoint()

        # Rollback to stable discards it again, leaving the first version current.
        self.conn.rollback_to_stable()

        # Force every leaf to reconcile and evict, so the next read comes off disk.
        ev = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        while ev.next() == 0:
            pass
        ev.close()

        wrong = []
        vc = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            vc.set_key(i)
            self.assertEqual(vc.search(), 0)
            if vc.get_value() != self.stable_value:
                wrong.append(i)
                if len(wrong) >= 5:
                    break
            vc.reset()
        vc.close()
        self.assertEqual(wrong, [],
                         f'keys reading back the rolled-back value: {wrong}')
