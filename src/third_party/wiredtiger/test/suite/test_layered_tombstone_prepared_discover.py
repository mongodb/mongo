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

# test_layered_tombstone_prepared_discover.py
#   End-to-end coverage for restoring a prepared colliding value through prepared discovery. A
#   value colliding with the tombstone marker is prepared on the leader and checkpointed with the
#   transaction still in flight, so the stable checkpoint carries the prepared cell (raw when
#   stable tombstone encoding is off, escaped when it is on). A follower picks up the checkpoint;
#   prepared discovery walks the stable image and restores the prepared update into the ingest
#   table, which must re-establish the ingest escape (__wt_clayered_stable_to_ingest_value) since
#   ingest-served values are always decoded. The transaction is then claimed and committed, and the
#   value must round-trip to its original bytes; the ingest constituent is read directly to prove
#   the restored bytes carry the escape.

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_tombstone_prepared_discover(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    ingest_uri = f'file:{test_name}.wt_ingest'

    conn_base_config = 'statistics=(all),precise_checkpoint=true,preserve_prepared=true,'

    values = [
        ('collide', dict(value=b'\x14\x14')),     # exactly the tombstone
        ('triple',  dict(value=b'\x14\x14\x14')), # tombstone prefix + a tombstone byte
        ('mixed',   dict(value=b'\x14\x14ab')),   # tombstone prefix + non-tombstone bytes
        ('normal',  dict(value=b'hello')),        # not in the namespace
    ]
    # Every node in the cluster runs the same mode; the restore behavior is what differs. The
    # escaped mode only arises on legacy data, so it is forced with the break-glass override; the
    # unescaped mode is what a new database adopts automatically.
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
        return self.conn_base_config + self.role_config('leader')

    # The bytes the restored update must hold on the ingest table: the ingest table always escapes
    # values in the tombstone namespace, whatever the stable mode.
    def expected_ingest_bytes(self):
        if self.value[:2] == b'\x14\x14':
            return self.value + b'\x14'
        return self.value

    def setUp(self):
        super().setUp()
        # A namespace value stored raw on the stable table logs a warning; it is expected and
        # benign here.
        self.ignoreStdoutPattern('stable table value in the tombstone namespace')

    def test_prepared_discover_round_trips_value(self):
        # Phase 1 (leader): commit a control key, then prepare the colliding value and checkpoint
        # with the transaction still in flight so the stable image carries the prepared cell.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(50))
        self.session.create(self.uri, 'key_format=i,value_format=u')
        c = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        c[2] = b'control'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(60))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(70))

        self.session.begin_transaction()
        c[1] = self.value
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(100) +
            ',prepared_id=' + self.prepared_id_str(1))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(150))
        ckpt_session = self.conn.open_session()
        ckpt_session.checkpoint()
        ckpt_session.close()
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        # Phase 2 (follower): pick up the checkpoint. Prepared discovery walks the stable image and
        # restores the prepared update into the ingest table.
        self.reopen_conn(config=self.conn_base_config +
            self.role_config('follower').rstrip(')') + f',checkpoint_meta="{checkpoint_meta}")')

        # Phase 3 (resolve): discover, claim and commit the prepared transaction.
        discover_cursor = self.session.open_cursor('prepared_discover:')
        claim_session = self.conn.open_session()
        discovered = []
        while discover_cursor.next() == 0:
            pid = discover_cursor.get_key()
            discovered.append(pid)
            claim_session.begin_transaction(
                'claim_prepared_id=' + self.prepared_id_str(pid))
        discover_cursor.close()
        self.assertEqual(discovered, [1])
        claim_session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(200) +
            ',durable_timestamp=' + self.timestamp_str(210))
        claim_session.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(220))

        # Phase 4 (verify): the restored, committed value round-trips to its original bytes and the
        # control key is intact.
        session_f = self.conn.open_session()
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

        # Phase 5 (in-memory proof): read the ingest constituent directly, bypassing the layered
        # decode. The restored update must carry the ingest escape whatever the stable mode.
        expected = self.expected_ingest_bytes()
        c = session_f.open_cursor(self.ingest_uri)
        session_f.begin_transaction('read_timestamp=' + self.timestamp_str(200))
        c.set_key(1)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), expected)
        session_f.rollback_transaction()
        c.close()
        session_f.close()
