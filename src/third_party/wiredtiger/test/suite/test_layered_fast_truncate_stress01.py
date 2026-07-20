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

import random, string, time
from collections import namedtuple
from enum import Enum

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# Long-running stress test for fast truncate correctness across step-up in
# disaggregated WiredTiger.
#
# Two disaggregated nodes share one database. Each round:
#   1. Apply a randomized mix of inserts, updates, removes, and range
#      truncates on the follower, mirroring every op into ValidationModel.
#   2. Switch roles -- promote the follower, restart the previous leader as
#      a fresh follower.
#   3. Verify the new leader against ValidationModel: full scan matches the
#      latest snapshot, and point reads at random past timestamps match the
#      mirrored history.
#
# FIXME-WT-17637: write-conflict scenarios are deliberately avoided. The
# round schedule filters insert/update/remove keys that fall inside any in-flight
# truncate range.
#
# The RNG seed is logged at the start so failures can be reproduced.

class operations(Enum):
    INSERT = 1
    UPDATE = 2
    REMOVE = 3
    TRUNCATE = 4

# Descriptor for an in-flight truncate transaction: the session and the two
# cursors holding the [lo, hi] range, kept open until commit.
OpenTruncate = namedtuple('OpenTruncate',
                          ['session', 'cursor_lo', 'cursor_hi', 'lo', 'hi'])

# Create validation model that mirrors the table contents. For every key
# we keep a history of all operations, so we can verify both the latest
# state and the value visible at any past timestamp.
HistoryEntry = namedtuple('HistoryEntry', ['ts', 'value'])

class ValidationModel:
    def __init__(self):
        self.history = {}
        self.max_ts = 0

    def latest(self, key):
        h = self.history.get(key)
        return h[-1].value if h else None

    def value_at(self, key, ts):
        v = None
        for entry in self.history.get(key, []):
            if entry.ts <= ts:
                v = entry.value
            else:
                break
        return v

    def insert(self, key, value, ts):
        self.history.setdefault(key, []).append(HistoryEntry(ts, value))
        self.max_ts = max(self.max_ts, ts)

    # A history entry's value is None when the key is deleted at that
    # timestamp; otherwise it's the string the user wrote.
    def is_present(self, key):
        return self.latest(key) is not None

    def remove(self, key, ts):
        if self.is_present(key):
            self.history.setdefault(key, []).append(HistoryEntry(ts, None))
            self.max_ts = max(self.max_ts, ts)

    def truncate(self, lo, hi, ts):
        for k in range(lo, hi + 1):
            if self.is_present(k):
                self.history.setdefault(k, []).append(HistoryEntry(ts, None))
        self.max_ts = max(self.max_ts, ts)

    def latest_snapshot(self):
        return {k: h[-1].value for k, h in self.history.items()
                if h[-1].value is not None}

    def assert_latest_matches(self, testcase, session, uri):
        cursor = session.open_cursor(uri)
        seen = {k: v for k, v in cursor}
        cursor.close()
        testcase.assertEqual(seen, self.latest_snapshot(),
            f'full-scan mismatch (seed={testcase.seed})')

    # Point reads at a few random past timestamps must match value_at(k, ts).
    # Tries to catch step-up bugs that break mvcc visibility for older timestamps.
    def assert_history_matches(self, testcase, session, uri,
                               sample_size=200, ts_samples=8):
        if self.max_ts <= 1 or not self.history:
            return
        keys = list(self.history.keys())
        sample_keys = random.sample(keys, min(sample_size, len(keys)))
        for _ in range(ts_samples):
            ts = random.randint(1, self.max_ts)
            session.begin_transaction(
                'read_timestamp=' + testcase.timestamp_str(ts))
            cursor = session.open_cursor(uri)
            for k in sample_keys:
                self._assert_point_read(testcase, cursor, k, ts)
            cursor.close()
            session.rollback_transaction()

    def _assert_point_read(self, testcase, cursor, key, ts):
        cursor.set_key(key)
        ret = cursor.search()
        expected = self.value_at(key, ts)
        if expected is None:
            testcase.assertEqual(ret, wiredtiger.WT_NOTFOUND,
                f'k={key} ts={ts} should be deleted (seed={testcase.seed})')
            return
        testcase.assertEqual(ret, 0,
            f'k={key} ts={ts} should be visible (seed={testcase.seed})')
        testcase.assertEqual(cursor.get_value(), expected,
            f'k={key} ts={ts} wrong value (seed={testcase.seed})')

@disagg_test_class
class test_layered_fast_truncate_stress01(wttest.WiredTigerTestCase):

    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader")'
    uri = f'layered:{test_name}'
    table_config = 'key_format=i,value_format=S,leaf_page_max=4096'

    # Scale workload by mode: a quick smoke run under the regular suite
    # (~10s target), and a long stress run under --long (~5min target).
    # Same pattern as test_cursor_bound_fuzz.
    nitems = 200000 if wttest.islongtest() else 5000
    rounds = 1600 if wttest.islongtest() else 160
    ops_per_round = 80 if wttest.islongtest() else 20
    truncates_per_round = 4 if wttest.islongtest() else 2
    max_truncate_width = 4096 if wttest.islongtest() else 256
    max_truncate_placement_tries = 5
    value_size = 16

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def follower_config(self):
        return self.extensionsConfig() + \
            ',create,statistics=(all),disaggregated=(role="follower")'

    def _gen_value(self):
        return ''.join(random.choice(string.ascii_lowercase)
                       for _ in range(self.value_size))

    def _next_ts(self):
        ts = self.next_ts
        self.next_ts += 1
        return ts

    # Create a random stream of inserts/removes/truncates operations at random positions.
    def _build_op_stream(self):
        stream = [random.choice([operations.INSERT, operations.UPDATE,
                                 operations.REMOVE])
                  for _ in range(self.ops_per_round)]
        for _ in range(self.truncates_per_round):
            stream.insert(random.randrange(len(stream) + 1),
                          operations.TRUNCATE)
        return stream

    def _overlaps_open(self, lo, hi, open_truncs):
        for t in open_truncs:
            if lo <= t.hi and t.lo <= hi:
                return True
        return False

    def _key_in_open(self, key, open_truncs):
        for t in open_truncs:
            if t.lo <= key <= t.hi:
                return True
        return False

    def _begin_truncate(self, follower_conn, open_truncs):
        # Pick a random [lo, hi] that does not overlap any currently-open
        # truncate range. Give up after max_truncate_placement_tries attempts.
        for _ in range(self.max_truncate_placement_tries):
            width = random.randint(1, self.max_truncate_width)
            lo = random.randint(0, self.nitems - width)
            hi = lo + width - 1
            if not self._overlaps_open(lo, hi, open_truncs):
                break
        else:
            return None

        sess = follower_conn.open_session('')
        cursor_lo = sess.open_cursor(self.uri)
        cursor_hi = sess.open_cursor(self.uri)
        cursor_lo.set_key(lo)
        cursor_hi.set_key(hi)
        sess.begin_transaction()
        sess.truncate(None, cursor_lo, cursor_hi, None)
        return OpenTruncate(sess, cursor_lo, cursor_hi, lo, hi)

    def _commit_open_truncate(self, open_truncs, ts):
        # Pop one open truncate and commit it at ts. Releases the range
        # claim so subsequent ops in this round can touch those keys.
        t = open_truncs.pop(random.randrange(len(open_truncs)))
        t.session.commit_transaction(
            'commit_timestamp=' + self.timestamp_str(ts))
        self.model.truncate(t.lo, t.hi, ts)
        t.cursor_lo.close()
        t.cursor_hi.close()
        t.session.close()

    def _do_write(self, session, op, key, ts):
        # Apply a single-key insert/update/remove at ts and mirror it in the
        # ValidationModel. insert only proceeds if the key is absent in the
        # model; update only proceeds if the key is present. remove no-ops
        # if the key is absent. The model update is deferred via mirror so
        # it only runs after commit_transaction succeeds.
        key_present = self.model.is_present(key)
        if op is operations.INSERT and key_present:
            return
        if op is operations.UPDATE and not key_present:
            return
        if op is operations.REMOVE and not key_present:
            return
        config = 'overwrite=false' if op is operations.REMOVE else 'overwrite=true'
        cursor = session.open_cursor(self.uri, None, config)
        try:
            session.begin_transaction()
            if op is operations.REMOVE:
                cursor.set_key(key)
                self.assertEqual(cursor.remove(), 0)
                mirror = lambda: self.model.remove(key, ts)
            else:
                value = self._gen_value()
                cursor[key] = value
                mirror = lambda: self.model.insert(key, value, ts)
            session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(ts))
            mirror()
        finally:
            cursor.close()

    def run_round(self, follower_conn, follow_session):
        open_truncs = []
        for op in self._build_op_stream():
            ts = self._next_ts()

            # Drain an open truncate first when one is queued, and always
            # before starting another truncate.
            if open_truncs and (op is operations.TRUNCATE or
                                random.random() < 0.4):
                self._commit_open_truncate(open_truncs, ts)
                continue

            if op is operations.TRUNCATE:
                t = self._begin_truncate(follower_conn, open_truncs)
                if t is not None:
                    open_truncs.append(t)
                continue

            key = random.randrange(self.nitems)
            if self._key_in_open(key, open_truncs):
                continue
            self._do_write(follow_session, op, key, ts)

        while open_truncs:
            self._commit_open_truncate(open_truncs, self._next_ts())

    # ---------- topology: leader / follower switch ----------

    def setup_follower(self):
        self.follower_dir = 'follower'
        self.leader_dir = '.'
        self.follower_conn = self.wiredtiger_open(
            self.follower_dir, self.follower_config())
        sess = self.follower_conn.open_session('')
        sess.create(self.uri, self.table_config)
        sess.close()
        self.disagg_advance_checkpoint(self.follower_conn)

    def switch_and_restart_follower(self):
        self.disagg_switch_follower_and_leader(self.follower_conn, self.conn)
        self.follower_conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(self.next_ts - 1))
        ckpt = self.follower_conn.open_session('')
        ckpt.checkpoint()
        ckpt.close()

        self.reopen_conn(directory=self.leader_dir,
                         config=self.follower_config())
        self.disagg_advance_checkpoint(self.conn, self.follower_conn)

        # Swap self.conn and self.follower_conn. The old leader is now the follower and vice versa.
        self.session.close()
        self.conn, self.follower_conn = self.follower_conn, self.conn
        self.leader_dir, self.follower_dir = self.follower_dir, self.leader_dir
        self.session = self.conn.open_session('')

    # ---------- main test ----------

    def seed_rng(self):
        self.seed = time.time()
        self.pr('Using seed: ' + str(self.seed))
        random.seed(self.seed)

    def populate_initial_leader(self):
        self.session.create(self.uri, self.table_config)
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nitems):
            v = self._gen_value()
            self.session.begin_transaction()
            cursor[i] = v
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(10))
            self.model.insert(i, v, 10)
        cursor.close()
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(10) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

    def test_stress_switch_cycles(self):
        self.seed_rng()
        self.model = ValidationModel()
        self.next_ts = 11
        self.round_idx = 0

        self.populate_initial_leader()
        self.setup_follower()

        # The follower may re-attach to the same checkpoint after a switch.
        self.ignoreStdoutPattern('Picking up the same checkpoint')
        self.ignoreStderrPatternIfExists('Picking up the same checkpoint')

        for self.round_idx in range(self.rounds):
            follow_session = self.follower_conn.open_session('')
            try:
                self.run_round(self.follower_conn, follow_session)
            finally:
                follow_session.close()

            self.switch_and_restart_follower()

            # Validate on the new leader against the ValidationModel.
            verify_session = self.conn.open_session('')
            try:
                self.model.assert_latest_matches(self, verify_session, self.uri)
                self.model.assert_history_matches(self, verify_session, self.uri)
            finally:
                verify_session.close()

        self.follower_conn.close()
