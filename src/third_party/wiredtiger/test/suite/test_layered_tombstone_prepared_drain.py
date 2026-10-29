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

# test_layered_tombstone_prepared_drain.py
#   End-to-end coverage for draining a prepared colliding value from the ingest table into the
#   stable table across a follower step-up. A value colliding with the tombstone marker is written
#   under a prepared transaction on a follower (escaped in the ingest table), the follower steps up
#   (draining the ingest table into the stable table with the prepared transaction still in flight),
#   and the transaction then commits. With stable tombstone encoding off the drain must strip the
#   escape so the stable on-disk image stays raw; with it on the escape is kept. Either way the value
#   round-trips to its original bytes. This complements test_layered_tombstone_drain, which drains
#   already-committed values, by exercising the prepared drain end to end with preserve_prepared
#   enabled. The drain copies the prepared value onto the stable btree through the standard value
#   path, so the on-disk bytes are produced by __wt_clayered_ingest_to_stable_value; commit then
#   resolves by key against those drain-allocated copies.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_tombstone_prepared_drain(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    stable_uri = f'file:{test_name}.wt_stable'

    conn_base_config = 'statistics=(all),precise_checkpoint=true,preserve_prepared=true,'

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
        # re-picks-up the last checkpoint; both are expected and benign here.
        self.ignoreStdoutPattern(
            'stable table value in the tombstone namespace|Picking up the same checkpoint again')

    def test_prepared_drain_round_trips_value(self):
        # Phase 1 (leader): create the table and checkpoint a base the follower can load, then close
        # the leader so the follower is the only live node when it steps up.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))
        self.session.create(self.uri, 'key_format=i,value_format=u')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))
        self.session.checkpoint()
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.conn.close('debug=(skip_checkpoint=true)')

        # Phase 2 (follower): load the checkpoint and prepare the colliding value. It lands in the
        # ingest table, escaped, as an in-flight prepared update.
        conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,' + self.conn_base_config + self.role_config('follower'))
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        # A committed control key on the same table confirms the prepared drain leaves unrelated data
        # intact.
        ctrl_session = conn_follow.open_session()
        cc = ctrl_session.open_cursor(self.uri)
        ctrl_session.begin_transaction()
        cc[2] = b'control'
        ctrl_session.commit_transaction('commit_timestamp=' + self.timestamp_str(80))
        cc.close()

        prep_session = conn_follow.open_session()
        c = prep_session.open_cursor(self.uri)
        prep_session.begin_transaction()
        c[1] = self.value
        c.close()
        prep_session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id=' + self.prepared_id_str(1))

        # Phase 3 (step up): the follower becomes leader. The step-up drains the ingest table into
        # the stable table with the prepared transaction still in flight, converting the escaped
        # ingest value to the stable table's form.
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # Phase 4 (resolve): commit the prepared transaction now that it lives on the stable table.
        prep_session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(200) +
            ',durable_timestamp=' + self.timestamp_str(210))
        prep_session.commit_transaction()

        conn_follow.set_timestamp('stable_timestamp=' + self.timestamp_str(250))
        session_f = conn_follow.open_session()
        session_f.checkpoint()

        # Phase 5 (verify): the drained, committed value round-trips to its original bytes, and a
        # verify walk of the stable table succeeds.
        c = session_f.open_cursor(self.uri)
        session_f.begin_transaction('read_timestamp=' + self.timestamp_str(200))
        c.set_key(1)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), self.value)
        c.set_key(2)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), b'control')
        session_f.rollback_transaction()
        c.close()
        session_f.verify(self.uri)

        # Phase 6 (on-disk proof): read the stable constituent directly, bypassing the layered
        # decode. The stored bytes carry the escape only when the mode is on and the value is in the
        # tombstone namespace, proving the prepared drain stripped (or kept) the escape as configured.
        expected = self.expected_stable_bytes()
        c = session_f.open_cursor(self.stable_uri)
        session_f.begin_transaction('read_timestamp=' + self.timestamp_str(200))
        c.set_key(1)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), expected)
        session_f.rollback_transaction()
        c.close()

        session_f.close()
        conn_follow.close()
