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

# test_layered_tombstone_drain.py
#   The ingest table always escapes values that collide with the tombstone marker. When the follower
#   steps up to leader it drains the ingest table into the stable table, and what happens to the
#   escape byte then depends on the stable tombstone encoding mode. With the mode off the drain must
#   strip the escape so it never reaches the stable on-disk image; with the mode on the drain keeps
#   the escape and the stable image stores it. Either way the value must round-trip to its original
#   bytes through a layered cursor. This test writes a colliding value on a follower (escaped in the
#   ingest table), steps the follower up (draining into the stable table), confirms the round-trip,
#   and inspects the raw stable constituent to prove the escape byte is absent on disk when the mode
#   is off and present when it is on.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_tombstone_drain(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    stable_uri = f'file:{test_name}.wt_stable'

    conn_base_config = 'statistics=(all),precise_checkpoint=true,'

    values = [
        ('collide', dict(value=b'\x14\x14')),     # exactly the tombstone
        ('triple',  dict(value=b'\x14\x14\x14')), # tombstone prefix + a tombstone byte
        ('mixed',   dict(value=b'\x14\x14ab')),   # tombstone prefix + non-tombstone bytes
        ('normal',  dict(value=b'hello')),        # not in the namespace
    ]
    # Every node in the cluster runs the same mode; the drain behavior is what differs. The escaped
    # mode only arises on legacy data, so it is forced with the break-glass override; the unescaped
    # mode is what a new database adopts automatically.
    modes = [
        ('escaped',   dict(encoding='true')),
        ('unescaped', dict(encoding='false')),
    ]
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, values, modes)

    def role_config(self, role):
        enc = 'legacy_tombstone_encoding_break_glass=true,' if self.encoding == 'true' else ''
        return f'disaggregated=({enc}role="{role}")'

    def conn_config(self):
        return self.extensionsConfig() + ',create,' + self.conn_base_config + self.role_config('leader')

    # The bytes the value is stored as on the stable table: the ingest escape survives the drain only
    # while the stable table also escapes, and only values in the tombstone namespace are escaped.
    def expected_stable_bytes(self):
        in_namespace = self.value[:2] == b'\x14\x14'
        if self.encoding == 'true' and in_namespace:
            return self.value + b'\x14'
        return self.value

    def setUp(self):
        super().setUp()
        # A namespace value stored raw on the stable table logs a warning, and the step-up
        # re-picks-up the leader's last checkpoint; both are expected and benign here.
        self.ignoreStdoutPattern(
            'stable table value in the tombstone namespace|Picking up the same checkpoint again')

    def get_stat(self, conn, which):
        s = conn.open_session()
        stat_cursor = s.open_cursor('statistics:')
        value = stat_cursor[which][2]
        stat_cursor.close()
        s.close()
        return value

    def test_drain_round_trips_value(self):
        # Phase 1 (leader): create the table and take a base checkpoint the follower can load.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.session.create(self.uri, 'key_format=i,value_format=u')
        self.session.checkpoint()

        # Phase 2 (follower): load the checkpoint and write the colliding value. It lands in the
        # ingest table, escaped.
        conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,' + self.conn_base_config + self.role_config('follower'))
        self.disagg_advance_checkpoint(conn_follow)
        # Key 2 is a plain control value, untouched by the escape logic, to confirm the drain leaves
        # unrelated keys intact.
        session_f = conn_follow.open_session()
        c = session_f.open_cursor(self.uri)
        session_f.begin_transaction()
        c[1] = self.value
        c[2] = b'control'
        session_f.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        c.close()
        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Phase 3 (step up): step the leader down and the follower up. The step-up drains the ingest
        # table into the stable table.
        self.disagg_switch_follower_and_leader(conn_follow)

        # Phase 4 (verify): the drained value round-trips to its original bytes, and a checkpoint +
        # verify walk of the stable table succeeds.
        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(30))
        session_f.checkpoint()
        c = session_f.open_cursor(self.uri)
        session_f.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        c.set_key(1)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), self.value)
        c.set_key(2)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), b'control')
        session_f.rollback_transaction()
        c.close()
        session_f.verify(self.uri)

        # The strip counter fires exactly when the drain drops an escape byte: unescaped mode with a
        # namespace value. This confirms the strip path independently of the on-disk byte check.
        in_namespace = self.value[:2] == b'\x14\x14'
        stripped = self.get_stat(conn_follow, wiredtiger.stat.conn.disagg_ingest_stable_tombstone_stripped)
        if self.encoding == 'false' and in_namespace:
            self.assertGreater(stripped, 0)
        else:
            self.assertEqual(stripped, 0)

        # Phase 5 (on-disk proof): read the stable constituent directly, bypassing the layered
        # decode. The stored bytes carry the escape only when the mode is on and the value is in the
        # tombstone namespace, proving the drain stripped (or kept) the escape as configured.
        expected = self.expected_stable_bytes()
        c = session_f.open_cursor(self.stable_uri)
        session_f.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        c.set_key(1)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), expected)
        session_f.rollback_transaction()
        c.close()

        session_f.close()
        conn_follow.close()
