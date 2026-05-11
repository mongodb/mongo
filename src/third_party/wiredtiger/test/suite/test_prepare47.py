#!/usr/bin/env python
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
#
# test_prepare47.py
#   Prepared insert on top of an existing tombstone, rolled back with the rollback
#   timestamp ahead of stable. The chain ends up with an unresolved aborted prepared
#   update at the head and the committed tombstone behind it. Reconcile must keep the
#   tombstone in the chain so a later reconcile that writes the prepared update has a
#   rollback fallback.

import wttest
from wtscenario import make_scenarios

class test_prepare47(wttest.WiredTigerTestCase):
    conn_config = 'precise_checkpoint=true,preserve_prepared=true'

    uri = 'table:test_prepare47'

    format_values = [
        ('row', dict(key_format='i')),
        ('column', dict(key_format='r')),
    ]
    scenarios = make_scenarios(format_values)

    def evict_key(self, evict_session, key, read_ts):
        evict_cursor = evict_session.open_cursor(
            self.uri, None, 'debug=(release_evict)')
        evict_session.begin_transaction(
            'ignore_prepare=true,read_timestamp=' + self.timestamp_str(read_ts))
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.reset()
        evict_session.rollback_transaction()
        evict_cursor.close()

    def read_key_at(self, sess, key, ts):
        c = sess.open_cursor(self.uri)
        sess.begin_transaction(
            'ignore_prepare=true,read_timestamp=' + self.timestamp_str(ts))
        c.set_key(key)
        ret = c.search()
        val = c.get_value() if ret == 0 else None
        sess.rollback_transaction()
        c.close()
        return val

    def test_aborted_prepared_with_committed_tombstone(self):
        # Ordered timestamps for the scenario:
        #   insert_ts < delete_ts < oldest_after_delete < stable_unstable
        #     < prepare_ts < stable_stable_prepare < rollback_ts
        insert_ts = 20
        delete_ts = 30
        oldest_after_delete = 31
        stable_unstable = 35           # below prepare_ts; prepare is unstable
        prepare_ts = 40
        stable_stable_prepare = 42     # above prepare_ts, below rollback_ts
        rollback_ts = 45

        self.session.create(
            self.uri,
            'key_format=' + self.key_format + ',value_format=S,'
            'allocation_size=512,leaf_page_max=4KB,memory_page_max=4KB')

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        cursor = self.session.open_cursor(self.uri)
        evict_session = self.conn.open_session()

        # Insert and force to disk so reconcile later sees the value in the cell.
        nrows = 100
        v_init = 'A' * 256
        self.session.begin_transaction()
        for k in range(1, nrows + 1):
            cursor[k] = v_init
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(insert_ts))
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(insert_ts + 1))
        self.session.checkpoint()
        for k in range(1, nrows + 1):
            self.evict_key(evict_session, k, insert_ts)

        # Delete in memory only; on-disk keeps v_init, chain head is the tombstone.
        self.session.begin_transaction()
        for k in range(1, nrows + 1):
            cursor.set_key(k)
            cursor.remove()
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(delete_ts))

        # Prepared insert on top of the tombstone.
        v_new = 'B' * 256
        self.session.begin_transaction()
        for k in range(1, nrows + 1):
            cursor[k] = v_new
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(prepare_ts) +
            ',prepared_id=' + self.prepared_id_str(99))
        cursor.reset()

        # Stable below prepare_ts: prepared update is unstable.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(stable_unstable))

        # Roll back with rollback_ts ahead of stable: rollback is also unstable.
        # Advance oldest past the tombstone so the tombstone is timestamp-visible.
        self.session.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(rollback_ts))
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(oldest_after_delete))

        # First eviction round: chain has [aborted_prepared, committed_tombstone],
        # tombstone gets selected for write.
        for k in range(1, nrows + 1):
            self.evict_key(evict_session, k, stable_unstable)

        # Advance stable past prepare_ts but stay below rollback_ts so the
        # aborted prepared update gets selected for a write_prepare reconcile.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(stable_stable_prepare))

        # Second eviction round: this is where the leaked-prepared-update
        # assertion fires without the fix.
        for k in range(1, nrows + 1):
            self.evict_key(evict_session, k, stable_stable_prepare)

        # Reads at any post-rollback timestamp should see the deletion.
        for k in (1, nrows // 2, nrows):
            v = self.read_key_at(evict_session, k, stable_stable_prepare)
            self.assertIsNone(v,
                f'expected NOT_FOUND for key {k} at ts={stable_stable_prepare}, got {v!r}')

        cursor.close()
        evict_session.close()

    def test_aborted_prepared_with_lost_disk_fallback(self):
        # Theory: at rollback time, first_committed_upd is NULL (no committed update behind
        # the prepared insert) but tw_found is true (on-disk cell with stop is the fallback),
        # so __txn_prepare_rollback_delete_key is not called and no rollback tombstone is
        # prepended. Later, a reconcile drops the on-disk cell (its stop is globally visible
        # and nothing is selected for the key), erasing the only fallback. A subsequent
        # reconcile that walks the surviving aborted prepared update has neither a rollback
        # tombstone nor an on-disk fallback, tripping the leaked-prepared-update assertion.
        insert_ts = 20
        delete_ts = 30
        oldest_after_delete = 31
        stable_unstable = 35
        prepare_ts = 40
        stable_post_prepare = 42
        rollback_ts = 45

        self.session.create(
            self.uri,
            'key_format=' + self.key_format + ',value_format=S,'
            'allocation_size=512,leaf_page_max=4KB,memory_page_max=4KB')

        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        cursor = self.session.open_cursor(self.uri)
        evict_session = self.conn.open_session()

        nrows = 100
        v_init = 'A' * 256

        # Insert + delete + evict so the on-disk cell carries both start and stop (the
        # form that can later be dropped when stop becomes globally visible).
        self.session.begin_transaction()
        for k in range(1, nrows + 1):
            cursor[k] = v_init
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(insert_ts))

        self.session.begin_transaction()
        for k in range(1, nrows + 1):
            cursor.set_key(k)
            cursor.remove()
        self.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(delete_ts))

        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(delete_ts + 1))
        self.session.checkpoint()
        for k in range(1, nrows + 1):
            self.evict_key(evict_session, k, insert_ts)

        # Prepared insert with no committed update behind it on the chain. The on-disk
        # cell (start+stop) is the only rollback fallback.
        v_new = 'B' * 256
        self.session.begin_transaction()
        for k in range(1, nrows + 1):
            cursor[k] = v_new
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(prepare_ts) +
            ',prepared_id=' + self.prepared_id_str(99))
        cursor.reset()

        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(stable_unstable))

        # Roll back with rollback_ts ahead of stable; first_committed_upd is NULL but
        # tw_found is true so no rollback tombstone is prepended.
        self.session.rollback_transaction(
            'rollback_timestamp=' + self.timestamp_str(rollback_ts))

        # Advance oldest past delete_ts so the on-disk stop becomes globally visible.
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(oldest_after_delete))

        # First eviction round at a stable below prepare_ts: aborted prepared is unstable
        # so it falls through the write_prepare path; on-disk cell may be rewritten.
        for k in range(1, nrows + 1):
            self.evict_key(evict_session, k, stable_unstable)

        # Advance stable past prepare_ts but stay below rollback_ts.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(stable_post_prepare))

        # Second eviction round: if the on-disk fallback was dropped, the leak assert
        # fires here for the surviving aborted prepared update.
        for k in range(1, nrows + 1):
            self.evict_key(evict_session, k, stable_post_prepare)

        for k in (1, nrows // 2, nrows):
            v = self.read_key_at(evict_session, k, stable_post_prepare)
            self.assertIsNone(v,
                f'expected NOT_FOUND for key {k} at ts={stable_post_prepare}, got {v!r}')

        cursor.close()
        evict_session.close()
