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

# A follower cursor may be positioned on a key that a newer checkpoint removes
# at a timestamp above the reader's. Moving to that checkpoint mid-iteration
# must transfer the position and continue without skipping or duplicating keys.
# This holds whether or not the follower has already locally applied the
# delete: a reader at an older read timestamp does not see a newer delete, so
# it stays positioned on the stable value regardless.

import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios


@disagg_test_class
class test_layered_cursor25(wttest.WiredTigerTestCase):
    test_name = __qualname__
    tablename = test_name
    uri = 'layered:' + tablename

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_base_config = 'cache_size=10MB,statistics=(all),precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    def test_iterate_across_checkpoint_dropping_positioned_key(self):
        # Leader writes three keys and takes a checkpoint.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        self.session.create(self.uri, 'key_format=i,value_format=S')
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c[1] = 'value_1'
        c[2] = 'value_2'
        c[3] = 'value_3'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

        # A follower picks up that checkpoint and can read all three keys.
        conn_f = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower)
        self.disagg_advance_checkpoint(conn_f)

        # Start iterating on the follower and stop while positioned on the first key.
        session_r = conn_f.open_session()
        cursor_r = session_r.open_cursor(self.uri)
        session_r.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        self.assertEqual(cursor_r.next(), 0)
        self.assertEqual(cursor_r.get_key(), 1)

        # The leader removes the positioned key above the reader's timestamp. Oldest stays at
        # the reader's timestamp: advancing it past an active reader would make the follower
        # refuse the adoption bind (WT-18408), and the newer checkpoint keeps the history the
        # reader needs.
        self.session.begin_transaction()
        c.set_key(1)
        self.assertEqual(c.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(25))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

        # Reflecting the mongo ordering, the follower locally applies the delete before
        # picking up the checkpoint that reflects it. This is the way an oplog-apply
        # cursor does it: an overwrite remove on a long-lived cursor that has already
        # read stable. The reader is pinned to an older timestamp, so the delete is
        # invisible to it and it stays positioned on the stable value.
        session_a = conn_f.open_session()
        cursor_a = session_a.open_cursor(self.uri, None, 'overwrite=true')
        # Read stable once so the overwrite remove can assume the key lives there.
        self.assertEqual(cursor_a.next(), 0)
        cursor_a.reset()
        session_a.begin_transaction()
        cursor_a.set_key(1)
        self.assertEqual(cursor_a.remove(), 0)
        session_a.commit_transaction('commit_timestamp=' + self.timestamp_str(25))
        cursor_a.close()
        session_a.close()

        # The follower moves to the newer checkpoint while the cursor stays positioned.
        self.disagg_advance_checkpoint(conn_f)

        # Continuing the iteration must not lose the position or crash. The remaining
        # keys are still returned in order.
        self.assertEqual(cursor_r.next(), 0)
        self.assertEqual(cursor_r.get_key(), 2)
        self.assertEqual(cursor_r.get_value(), 'value_2')

        self.assertEqual(cursor_r.next(), 0)
        self.assertEqual(cursor_r.get_key(), 3)
        self.assertEqual(cursor_r.get_value(), 'value_3')

        self.assertEqual(cursor_r.next(), wiredtiger.WT_NOTFOUND)

        session_r.rollback_transaction()
        cursor_r.close()
        session_r.close()
        conn_f.close()
