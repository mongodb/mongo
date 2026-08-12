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

# test_layered_async_stepdown10.py
#    Stress the async step-down: one thread walks the connection through the step-down phases
#    while a workload thread hammers creates, drops, inserts, removes and reads. The step-down
#    checkpoint is verified from a fresh follower before the node steps back up, and after a
#    step-up every surviving table must serve its exact contents, current and at the step-down
#    timestamp, to the leader and to a fresh follower. Runs in both the schema-epoch and the
#    epoch-less world.

import collections, itertools, random, threading, time, traceback
import wiredtiger, wtthread, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_async_stepdown10(
  LayeredStepdownMixin, wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__

    table_config = 'key_format=S,value_format=S'

    base = 'statistics=(all),precise_checkpoint=true,cache_size=500MB,'
    conn_config = base + 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = base + 'disaggregated=(role="follower",lose_all_my_data=true)'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    # The two worlds share the connection configs and differ only in the schema epochs.
    worlds = [
      ('epoch', dict(use_epochs=True)),
      ('legacy', dict(use_epochs=False)),
    ]
    scenarios = make_scenarios(disagg_storages, worlds)

    # How long the workload runs against each phase of the transition.
    phase_sleep = 10.0 if wttest.islongtest() else 1.0

    # Bound the table population so the final verification stays cheap.
    table_cap = 100 if wttest.islongtest() else 40

    initial_stable_epoch = 10

    def uri(self, name):
        return f'layered:{self.test_name}_{name}'

    def alloc_ts(self):
        with self.ts_lock:
            return next(self.ts_counter)

    def setup_stress_state(self):
        # Commit timestamps and the step-down timestamp come from one counter and every
        # timestamp call happens under one lock, so a commit can never trail the step-down or
        # stable timestamp. The only legal commit failure is the straddle rollback: the engine
        # rolling back a transaction that was in flight when the step-down timestamp landed.
        self.ts_lock = threading.Lock()
        self.ts_counter = itertools.count(self.initial_stable_epoch)
        # Publish epochs stay above the initial stable epoch, so every publish is legal.
        self.epoch_counter = itertools.count(self.initial_stable_epoch + 10)
        self.name_counter = itertools.count()
        # Ground truth, only written by the workload thread: uri -> the per-commit history
        # (commit_ts -> rows, a None value is a remove), the epoch the create was published at
        # (None in the epoch-less world), and whether the create fell inside the window.
        self.tables = {}
        self.seed_uris = set()
        self.drops_to_verify = set()
        self.op_counts = collections.Counter()
        self.workload_errors = []
        self.step_down_ts = None
        self.demotion_started = False
        self.done = threading.Event()

    # Whether a drop is guaranteed to reach the shared metadata, so the final verification may
    # insist the table is gone from it.
    def drop_removal_is_guaranteed(self):
        # Once demotion starts, this single-process harness has no relay to a leader.
        if self.demotion_started:
            return False
        # The epoch-less step-up rebuilds shared metadata from a best-effort local scan, which
        # cannot see a table dropped after the last leader checkpoint.
        if not self.use_epochs and self.step_down_ts is not None:
            return False
        return True

    # Publish a schema change in the epoch world, at a fresh epoch unless the caller pins one.
    def publish_if_epochs(self, uri, epoch=None, session=None):
        if not self.use_epochs:
            return None
        if epoch is None:
            epoch = next(self.epoch_counter)
        self.publish(uri, epoch, session=session)
        return epoch

    # A table is covered once its publish epoch is at or below the stable schema epoch. Create a
    # few covered, checkpointed tables: in the epoch world only these may be written below the
    # step-down timestamp, because a checkpoint refuses stable data on an uncovered table.
    def setup_seed_tables(self):
        if self.use_epochs:
            self.set_stable_epoch(self.initial_stable_epoch)
        self.set_global_ts(1, 1)

        for _ in range(3):
            uri = self.uri(f'w{next(self.name_counter)}')
            self.session.create(uri, self.table_config)
            epoch = self.publish_if_epochs(uri)
            rows = {f'k{n}': 'seed' for n in range(5)}
            ts = self.alloc_ts()
            self.write_at(uri, rows, ts)
            self.tables[uri] = {'history': [(ts, rows)], 'publish_epoch': epoch, 'window': False}
            self.seed_uris.add(uri)

        if self.use_epochs:
            self.set_stable_epoch(
                max(info['publish_epoch'] for info in self.tables.values()))
        self.leader_checkpoint(self.alloc_ts())

    def workload_create(self, wsession, rng):
        # Raise the cap once the window opens, so window creates still happen when the table
        # population filled up before the window.
        cap = self.table_cap + (20 if self.step_down_ts is not None else 0)
        if len(self.tables) >= cap:
            return
        uri = self.uri(f'w{next(self.name_counter)}')
        # The driver sets the timestamp while holding this lock, so if it is set here, the
        # engine already has it and this create is guaranteed to run inside the window.
        with self.ts_lock:
            ts_set_before_create = self.step_down_ts is not None
        # Creates and publishes tolerate no errors: a failure fails the test.
        wsession.create(uri, self.table_config)
        ts_set_after_create = self.step_down_ts is not None
        epoch = self.publish_if_epochs(uri, session=wsession)
        # Classify by the create's own outcome, which is exact even when the create races the
        # driver setting the timestamp: a window create builds no stable constituent.
        window = not self.stable_constituent_exists(self.conn, uri)
        # Both timestamp reads are one-sided, so each validates the outcome in one direction: a
        # create begun after the timestamp was set must skip the constituent, and one that
        # finished before it was set must build it.
        if ts_set_before_create:
            self.assertTrue(window, f'{uri} built a stable constituent inside the window')
        elif not ts_set_after_create:
            self.assertFalse(window, f'{uri} skipped its stable constituent outside the window')
        self.tables[uri] = {'history': [], 'publish_epoch': epoch, 'window': window}
        self.op_counts['window_creates' if window else 'creates'] += 1

    def workload_drop(self, wsession, rng):
        # Keep a couple of tables alive so writes and reads always have a target.
        if len(self.tables) <= 2:
            return
        uri = rng.choice(list(self.tables))
        try:
            # A single attempt: a table with unpublished data stays EBUSY until a checkpoint the
            # workload never takes, so a retry loop would live-lock.
            wsession.drop(uri, None)
        except wiredtiger.WiredTigerError as e:
            if not self.is_busy(e):
                raise
            # Only a transient conflict or unpublished data may refuse the drop.
            self.assertIn(wsession.get_last_error()[1],
                (wiredtiger.WT_DIRTY_DATA, wiredtiger.WT_CONFLICT_DHANDLE,
                 wiredtiger.WT_CONFLICT_TABLE_LOCK), f'drop of {uri}')
            self.op_counts['busy_drops'] += 1
            return
        info = self.tables.pop(uri)
        if self.drop_removal_is_guaranteed():
            self.drops_to_verify.add(uri)
        # Publish at the create's own epoch so the queued create/remove pair cancels rather than
        # leaving a create the covering checkpoint has no data for. A seed table's create left
        # the queue long ago, so its drop takes a fresh epoch instead.
        self.publish_if_epochs(
            uri, epoch=None if uri in self.seed_uris else info['publish_epoch'],
            session=wsession)
        self.op_counts['drops'] += 1

    # Choose the table for the next write. In the epoch world, until the window opens any commit
    # may become stable, which a checkpoint refuses on an uncovered table, so only the covered
    # seed tables are safe. Inside the window, commits land above the cutoff and any table works.
    def choose_write_table(self, rng):
        with self.ts_lock:
            window_open = self.step_down_ts is not None
        if self.use_epochs and not window_open:
            candidates = [u for u in self.tables if u in self.seed_uris]
        else:
            candidates = list(self.tables)
        return rng.choice(candidates) if candidates else None

    def commit_at_next_ts(self, wsession):
        with self.ts_lock:
            ts = next(self.ts_counter)
            wsession.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        return ts

    # Run one write transaction applying kvs to the table, where a None value removes the key,
    # and record it in the history if it commits. Tolerates only rollbacks with a known reason.
    def write_txn(self, wsession, rng, uri, kvs):
        cursor = wsession.open_cursor(uri)
        wsession.begin_transaction()
        ts = None
        resolved = False
        try:
            for k, v in kvs.items():
                if v is None:
                    cursor.set_key(k)
                    cursor.remove()
                else:
                    cursor[k] = v
            # Hold some transactions open so a phase boundary lands on them in flight.
            if rng.random() < 0.3:
                time.sleep(rng.uniform(0, 0.2))
            resolved = True
            # Occasionally abort, so both resolution paths cross the transition.
            if rng.random() < 0.1:
                wsession.rollback_transaction()
            else:
                ts = self.commit_at_next_ts(wsession)
        except wiredtiger.WiredTigerError as e:
            if not self.is_rollback(e):
                raise
            # Read the reason first: any later session call resets it.
            message = wsession.get_last_error()[2]
            # A failed commit has already resolved the transaction, an earlier failure has not.
            if not resolved:
                wsession.rollback_transaction()
            # A write may only be rolled back by the transition itself: a straddle of the
            # step-down boundary, or the demotion adopting the step-down checkpoint underneath a
            # transaction begun before it.
            if 'straddled the step-down timestamp' in message:
                self.op_counts['straddle_rollbacks'] += 1
            else:
                self.assertIn('A newer checkpoint was adopted', message,
                    f'write to {uri} rolled back with unexpected reason: {message}')
                self.assertTrue(self.demotion_started,
                    f'write to {uri} hit a checkpoint adoption before the demotion')
                self.op_counts['adoption_rollbacks'] += 1
        cursor.close()
        if ts is not None:
            self.tables[uri]['history'].append((ts, kvs))
            self.op_counts['commits'] += 1

    def workload_insert(self, wsession, rng):
        uri = self.choose_write_table(rng)
        if uri is None:
            return
        kvs = {f'k{rng.randrange(100)}': f'v{n}' for n in range(rng.randrange(1, 11))}
        self.write_txn(wsession, rng, uri, kvs)

    def workload_remove(self, wsession, rng):
        uri = self.choose_write_table(rng)
        if uri is None:
            return
        present = list(self.rows_at(self.tables[uri]))
        if present:
            kvs = {k: None for k in rng.sample(present, min(3, len(present)))}
            self.write_txn(wsession, rng, uri, kvs)

    # The rows a snapshot at read_ts must see, replayed from the commit history. With no
    # timestamp, everything committed.
    def rows_at(self, info, read_ts=None):
        rows = {}
        for ts, kvs in info['history']:
            if read_ts is None or ts <= read_ts:
                for k, v in kvs.items():
                    if v is None:
                        rows.pop(k, None)
                    else:
                        rows[k] = v
        return rows

    # Pick a read point and the rows it must see: the newest state, or a historical snapshot at
    # one of the table's own commit timestamps. Exact because this thread is the only writer.
    def pick_read_point(self, info, rng):
        if info['history'] and rng.random() < 0.5:
            ts = rng.choice(info['history'])[0]
            return self.rows_at(info, ts), 'read_timestamp=' + self.timestamp_str(ts)
        return self.rows_at(info), None

    def workload_read(self, wsession, rng):
        if not self.tables:
            return
        uri = rng.choice(list(self.tables))
        expected, config = self.pick_read_point(self.tables[uri], rng)

        cursor = wsession.open_cursor(uri)
        wsession.begin_transaction(config)
        # Hold some snapshots open across a phase boundary before reading through them.
        if rng.random() < 0.2:
            time.sleep(rng.uniform(0, 0.2))
        actual = None
        try:
            actual = {k: v for k, v in cursor}
        except wiredtiger.WiredTigerError as e:
            if not self.is_rollback(e):
                raise
            # A snapshot taken before the transition dies when the demotion adopts the step-down
            # checkpoint, and verifies nothing. No other reason may roll a read back.
            self.assertIn('A newer checkpoint was adopted', wsession.get_last_error()[2],
                f'read of {uri} rolled back for an unexpected reason')
            self.assertTrue(self.demotion_started,
                f'read of {uri} hit a checkpoint adoption before the demotion')
            self.op_counts['adoption_rollbacks'] += 1
        wsession.rollback_transaction()
        cursor.close()
        if actual is not None:
            self.assertEqual(actual, expected, f'{uri} served the wrong rows')
            self.op_counts['verified_reads'] += 1

    def workload_move_stable(self, wsession, rng):
        # Keep stable moving like a live system, but only until the step-down timestamp exists:
        # after that, stable belongs to the transition and must not pass the cutoff.
        with self.ts_lock:
            if self.step_down_ts is None:
                self.conn.set_timestamp(
                    'stable_timestamp=' + self.timestamp_str(next(self.ts_counter)))

    # Hammer a random mix of operations until told to stop. Unexpected errors are recorded and
    # fail the test after the join: an exception on this thread cannot fail the test by itself.
    def workload(self):
        ops = {
          'insert': (45, self.workload_insert),
          'read': (20, self.workload_read),
          'create': (12, self.workload_create),
          'remove': (10, self.workload_remove),
          'drop': (8, self.workload_drop),
          'stable': (5, self.workload_move_stable),
        }
        wsession = self.conn.open_session('')
        rng = random.Random(42)
        op = None
        try:
            while not self.done.is_set():
                time.sleep(0.002)
                op = rng.choices(list(ops), weights=[w for w, _ in ops.values()])[0]
                ops[op][1](wsession, rng)
        except Exception:
            self.workload_errors.append(
                f'{op}: {traceback.format_exc()}\nlast error: {wsession.get_last_error()}')
        finally:
            wsession.close()

    # Walk through every step-down phase, pausing after each so the workload runs against it.
    def step_down_in_phases(self):
        # Phase 0: a plain leader.
        time.sleep(self.phase_sleep)

        # Phase 1: open the window by setting the step-down timestamp.
        with self.ts_lock:
            self.step_down_ts = next(self.ts_counter)
            self.set_step_down_ts(self.step_down_ts)
        time.sleep(self.phase_sleep)

        # Phase 2: advance stable to the cutoff.
        with self.ts_lock:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.step_down_ts))
        time.sleep(self.phase_sleep)

        # Phase 3: take the final leader checkpoint at the cutoff.
        ckpt_session = self.conn.open_session()
        ckpt_session.checkpoint()
        ckpt_session.close()
        time.sleep(self.phase_sleep)

        # Phase 4: demote to follower, and let the workload run against the demoted node.
        self.demotion_started = True
        self.step_down()
        time.sleep(self.phase_sleep)

    # The workload must have made progress on every unconditional operation, or the stress and
    # its assertions were vacuous.
    def assert_workload_progress(self):
        self.pr(f'workload operations: {dict(self.op_counts)}, '
                f'drops to verify: {len(self.drops_to_verify)}')
        for counter in ('commits', 'verified_reads', 'creates', 'window_creates', 'drops'):
            self.assertGreater(self.op_counts[counter], 0, f'workload made no {counter}')
        # The engine and the workload must agree on which rollbacks were straddles.
        self.assertEqual(self.get_step_down_rollback_count(),
            self.op_counts['straddle_rollbacks'])

    # Verify the step-down checkpoint itself, before this node steps back up and re-covers
    # everything: a fresh follower must serve exactly the pre-window rows at the step-down
    # timestamp, and must not see the tables the checkpoint could not include, that is window
    # creates and, in the epoch world, everything but the covered seed tables.
    def verify_step_down_checkpoint(self):
        conn_follow, session_follow = self.open_follower()
        for uri, info in self.tables.items():
            in_checkpoint = not info['window'] and (not self.use_epochs or uri in self.seed_uris)
            if in_checkpoint:
                self.assertEqual(
                    self.read_kvs_at(uri, self.step_down_ts, session=session_follow),
                    self.rows_at(info, self.step_down_ts),
                    f'{uri} read wrong at the cutoff from the step-down checkpoint')
            else:
                self.assertFalse(self.uri_in_shared_metadata(conn_follow, uri),
                    f'{uri} advertised by the step-down checkpoint')
                # The checkpoint excluded this table, so the follower cannot open it either.
                self.assertRaises(wiredtiger.WiredTigerError,
                    lambda: session_follow.open_cursor(uri))
        self.close_follower(conn_follow, session_follow)

    # Step back up and take checkpoints covering everything the workload published. Drops are
    # two-phase, so the second checkpoint makes them durable in the shared metadata.
    def step_up_and_cover(self):
        self.step_up()
        # The demotion must have cleared the step-down timestamp.
        self.assertEqual(self.step_down_ts_is_set(), 0)
        if self.use_epochs:
            self.set_stable_epoch(next(self.epoch_counter))
        final_ts = self.alloc_ts()
        self.leader_checkpoint(final_ts)
        self.leader_checkpoint(self.alloc_ts())
        return final_ts

    # Every surviving table serves its exact rows through the given session, both the newest
    # state and the state at the step-down timestamp.
    def assert_rows_served(self, final_ts, session, where):
        for uri, info in self.tables.items():
            self.assertEqual(self.read_kvs_at(uri, final_ts, session=session),
                self.rows_at(info), f'{uri} served the wrong rows on the {where}')
            self.assertEqual(self.read_kvs_at(uri, self.step_down_ts, session=session),
                self.rows_at(info, self.step_down_ts),
                f'{uri} served the wrong rows at the cutoff on the {where}')

    # Every surviving table is fully published (constituent, checkpointed, advertised), no
    # unexpected table exists, and every drop with a guaranteed outcome left the shared metadata.
    def verify_leader_state(self, final_ts):
        self.assert_rows_served(final_ts, self.session, 'leader')
        for uri in self.tables:
            self.assert_table_state(self.conn, uri, True, True, True)
        self.assert_no_unexpected_tables(self.conn, list(self.tables))
        for uri in self.drops_to_verify:
            self.assertFalse(self.uri_in_shared_metadata(self.conn, uri),
                f'dropped {uri} still advertised in the shared metadata')

    # A fresh follower picking up the covering checkpoint serves every surviving table's rows.
    def verify_follower_reads(self, final_ts):
        conn_follow, session_follow = self.open_follower()
        self.assert_rows_served(final_ts, session_follow, 'fresh follower')
        self.close_follower(conn_follow, session_follow)

    def test_stepdown_under_workload(self):
        self.setup_stress_state()
        self.setup_seed_tables()

        worker = wtthread.Thread(target=self.workload)
        worker.start()
        try:
            self.step_down_in_phases()
        finally:
            self.done.set()
            worker.join()

        self.assertEqual(self.workload_errors, [])
        self.assert_workload_progress()

        self.verify_step_down_checkpoint()
        final_ts = self.step_up_and_cover()
        self.verify_leader_state(final_ts)
        self.verify_follower_reads(final_ts)
