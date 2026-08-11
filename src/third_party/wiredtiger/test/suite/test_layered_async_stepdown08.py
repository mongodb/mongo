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

# test_layered_async_stepdown08.py
#    Layered table state across the step-down transition: whether a table has a stable constituent,
#    whether that constituent was checkpointed, and whether the shared metadata advertises it. A
#    table created after the step-down timestamp has no stable constituent at all. Every test runs in
#    both the schema-epoch and the epoch-less world, which expect different states.

import threading, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, DisaggSchemaEpochMixin
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_async_stepdown08(
  LayeredStepdownMixin, wttest.WiredTigerTestCase, DisaggSchemaEpochMixin):
    test_name = __qualname__

    table_config = 'key_format=S,value_format=S'

    # Both worlds run with precise checkpoints, which disaggregated storage expects even from
    # clients that never publish. Only the schema epochs differ between the two worlds.
    base = 'statistics=(all),precise_checkpoint=true,'
    leader = 'disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = base + 'disaggregated=(role="follower",lose_all_my_data=true)'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    worlds = [
      ('epoch', dict(use_epochs=True, conn_config=base + leader)),
      ('legacy', dict(use_epochs=False, conn_config=base + leader)),
    ]
    scenarios = make_scenarios(disagg_storages, worlds)

    # The epoch a create is published at, above the step-down checkpoint's epoch so the create stays
    # uncovered in the epoch world.
    uncovered_epoch = 30

    def uri(self, name):
        return f'layered:{self.test_name}_{name}'

    # The step-down timestamp every test splits its work on.
    cutoff = 5

    def setup_world(self):
        """Configure the stable schema epoch only in the epoch world."""
        if self.use_epochs:
            self.set_stable_epoch(10)
        self.set_global_ts(1, 1)

    def publish_if_epochs(self, uri, epoch):
        """Publish a create, which the epoch-less world has no notion of."""
        if self.use_epochs:
            self.publish(uri, epoch)

    def publish_and_make_stable(self, uri, epoch):
        """Publish a create and advance the stable schema epoch to it, so a checkpoint covers it."""
        if self.use_epochs:
            self.publish(uri, epoch)
            self.set_stable_epoch(epoch)

    def enter_window(self):
        """Set up the world, then open the step-down window by setting the timestamp."""
        self.setup_world()
        self.set_step_down_ts(self.cutoff)

    def step_down_checkpoint(self):
        """
        Take the final leader checkpoint at the step-down timestamp. Everything committed at or below
        the cutoff becomes durable here; the rows written above it belong to the follower era.
        """
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.cutoff))
        self.session.checkpoint()

    def checkpoint_covering_epoch(self, epoch, stable_ts):
        """Take a checkpoint that covers everything published at or below the given epoch."""
        if self.use_epochs:
            self.set_stable_epoch(epoch)
        self.leader_checkpoint(stable_ts)

    def create_with_rows(self, name, commit_ts):
        """Create a table and write two rows, returning its URI and contents."""
        uri = self.uri(name)
        self.session.create(uri, self.table_config)
        rows = {'k1': name, 'k2': name}
        self.write_at(uri, rows, commit_ts)
        return uri, rows

    def assert_follower_reads(self, uri, expected):
        """A fresh follower picking up the latest checkpoint reads the expected contents."""
        conn_follow, session_follow = self.open_follower()
        cursor = session_follow.open_cursor(uri)
        self.assertEqual({k: v for k, v in cursor}, expected)
        cursor.close()
        self.close_follower(conn_follow, session_follow)

    def local_metadata_keys(self, conn, uri):
        """The local metadata keys naming the table or its constituents."""
        tablename = uri[len('layered:'):]
        session = conn.open_session('')
        cursor = session.open_cursor('metadata:')
        keys = [k for k, _ in cursor if tablename in k]
        cursor.close()
        session.close()
        return keys

    def test_constituent_presence_follows_the_timestamp(self):
        """
        Which side of the step-down timestamp a table was created on decides whether it has a stable
        constituent, and the transition changes neither answer. Both tables keep serving their rows,
        so "no constituent" never means "no data".
        """
        self.setup_world()
        before, before_rows = self.create_with_rows('before', 2)
        self.publish_and_make_stable(before, 20)

        self.set_step_down_ts(self.cutoff)
        after, after_rows = self.create_with_rows('after', 6)
        self.publish_if_epochs(after, 40)

        def assert_both_sides():
            self.assertTrue(self.stable_constituent_exists(self.conn, before))
            self.assertFalse(self.stable_constituent_exists(self.conn, after))
            # Assert the ingest constituent too, so the absent one cannot pass as a failed create.
            self.assertTrue(self.uri_in_local_metadata(self.conn, after))
            self.assertEqual(self.read_kvs_at(before, 7), before_rows)
            self.assertEqual(self.read_kvs_at(after, 7), after_rows)

        assert_both_sides()
        self.complete_step_down(self.cutoff)
        assert_both_sides()

    def create_tables_with_mixed_states(self):
        """
        Create a table for each state that can be alive at a step-down, take the step-down
        checkpoint, and return the URIs. Rows are written above that checkpoint's stable timestamp,
        which is what keeps an unpublished table legal in the epoch world.
        """
        self.setup_world()

        # Published below the cutoff and covered by the checkpoint that follows.
        covered, covered_rows = self.create_with_rows('covered', 2)
        self.publish_and_make_stable(covered, 20)

        # Published above the checkpoint's epoch, so the epoch world leaves it uncovered.
        uncovered = self.uri('uncovered')
        self.session.create(uncovered, self.table_config)
        self.publish_if_epochs(uncovered, self.uncovered_epoch)

        # Created and never published.
        unpublished = self.uri('unpublished')
        self.session.create(unpublished, self.table_config)

        self.set_step_down_ts(self.cutoff)

        # Created after the step-down timestamp. Its publish epoch has to exceed the stable schema
        # epoch the covered table advanced to, so the epoch world defers this entry by epoch and the
        # epoch-less world reaches it with no stable value to publish.
        window, window_rows = self.create_with_rows('window', 6)
        self.publish_if_epochs(window, 40)

        self.step_down_checkpoint()
        return {
          'covered': (covered, covered_rows),
          'uncovered': (uncovered, {}),
          'unpublished': (unpublished, {}),
          'window': (window, window_rows),
        }

    def assert_mixed_states(self, tables):
        """
        Assert all three states of every table. The epoch world withholds an unpublished table from
        the checkpoint and from shared metadata; the epoch-less world has no notion of publication,
        so it covers everything it has a constituent for.
        """
        covered, _ = tables['covered']
        uncovered, _ = tables['uncovered']
        unpublished, _ = tables['unpublished']
        window, _ = tables['window']

        self.assert_table_state(self.conn, covered, True, True, True)

        if self.use_epochs:
            self.assert_table_state(self.conn, uncovered, True, False, False)
            self.assert_table_state(self.conn, unpublished, True, False, False)
        else:
            self.assert_table_state(self.conn, uncovered, True, True, True)
            self.assert_table_state(self.conn, unpublished, True, True, True)

        # A window create has no constituent to checkpoint or advertise, in either world.
        self.assert_table_state(self.conn, window, False, False, False)

    def test_step_down_audit_mixed_states(self):
        """
        Every state a layered table can be in, alive at once and audited across the transition. The
        step-down checkpoint must cover exactly the tables it is supposed to and nothing else, and a
        per-table check only covers the tables it names, so enumerate as well: no table may change
        state underneath the step-down.
        """
        tables = self.create_tables_with_mixed_states()
        uris = [uri for uri, _ in tables.values()]

        self.assert_mixed_states(tables)
        self.assert_no_unexpected_tables(self.conn, uris)

        # A second checkpoint in the window changes nothing, which is what keeps the requeue of a
        # window create from consuming an entry it has to put back.
        self.session.checkpoint()
        self.assert_mixed_states(tables)

        self.step_down()

        self.assert_mixed_states(tables)
        self.assert_no_unexpected_tables(self.conn, uris)

    def test_follower_serves_tables_after_step_down(self):
        """
        After the transition the node reads in its follower role. The metadata state has to match
        what a reader sees: a covered table serves its rows, a window create serves them from the
        ingest constituent, and a constituent with no checkpoint reads empty.
        """
        tables = self.create_tables_with_mixed_states()
        self.step_down()

        for name in ('covered', 'window'):
            uri, rows = tables[name]
            self.assertEqual(self.read_kvs_at(uri, 7), rows, f'{name} did not serve its rows')

        # The epoch world left this constituent without a checkpoint, so a follower has nothing to
        # open for it. The epoch-less world checkpointed it, so it reads as the empty table it is.
        uncovered, _ = tables['uncovered']
        self.assertEqual(self.read_kvs_at(uncovered, 7), {})

    def test_timestampless_step_down_keeps_constituents(self):
        """
        A step-down with no timestamp set is the abrupt path, and it must keep constituents too:
        one covered by a checkpoint and one the checkpoint never reached.
        """
        self.setup_world()
        covered, rows = self.create_with_rows('covered', 2)
        self.publish_if_epochs(covered, 20)
        self.checkpoint_covering_epoch(20, 3)

        uncovered = self.uri('uncovered')
        self.session.create(uncovered, self.table_config)

        self.step_down()

        self.assertTrue(self.stable_constituent_exists(self.conn, covered))
        self.assertTrue(self.stable_constituent_exists(self.conn, uncovered))
        self.assertEqual(self.read_kvs_at(covered, 4), rows)

    def test_window_create_publishes_after_step_up(self):
        """
        A step-up builds the constituent a window create skipped while leaving alone the surviving
        one of a create that predates the timestamp, then a covering checkpoint publishes both with
        their rows. The two worlds reach this through different code: the epoch world replays the
        surviving queue entry, the epoch-less world rebuilds from a local metadata scan.
        """
        self.setup_world()
        before, before_rows = self.create_with_rows('before', 2)
        # Published below the cutoff, so the step-down checkpoint covers it and its rows.
        self.publish_and_make_stable(before, 20)

        self.set_step_down_ts(self.cutoff)
        after, after_rows = self.create_with_rows('after', 6)
        self.publish_if_epochs(after, 40)
        self.assertTrue(self.stable_constituent_exists(self.conn, before))
        self.assert_table_state(self.conn, after, False, False, False)

        self.complete_step_down(self.cutoff)
        self.assertTrue(self.stable_constituent_exists(self.conn, before))
        self.assert_table_state(self.conn, after, False, False, False)

        self.step_up()
        self.assertTrue(self.stable_constituent_exists(self.conn, before))
        self.assertTrue(self.stable_constituent_exists(self.conn, after))

        self.checkpoint_covering_epoch(40, 7)
        self.assert_table_state(self.conn, before, True, True, True)
        self.assert_table_state(self.conn, after, True, True, True)
        self.assert_follower_reads(before, before_rows)
        self.assert_follower_reads(after, after_rows)

    def test_window_create_matches_follower_create(self):
        """
        A window create is meant to leave the table in the state a create on a follower produces.
        Compare the local metadata keys of the two directly.
        """
        self.setup_world()
        self.leader_checkpoint(2)

        uri = self.uri('shape')
        conn_follow, session_follow = self.open_follower()
        session_follow.create(uri, self.table_config)
        follower_keys = sorted(self.local_metadata_keys(conn_follow, uri))
        self.close_follower(conn_follow, session_follow)

        self.set_step_down_ts(self.cutoff)
        self.session.create(uri, self.table_config)
        self.assertEqual(sorted(self.local_metadata_keys(self.conn, uri)), follower_keys)
        self.complete_step_down(self.cutoff)

    def test_multiple_window_creates_requeue(self):
        """
        A checkpoint inside the window meets every window create at once with no stable constituent
        to publish. They must all be deferred to the next leader era rather than reported as a
        violation, and a later covering checkpoint must publish all of them.
        """
        self.enter_window()

        tables = []
        for i in range(3):
            uri, rows = self.create_with_rows(f'many{i}', 6)
            self.publish_if_epochs(uri, 20)
            tables.append((uri, rows))

        self.step_down_checkpoint()
        for uri, _ in tables:
            self.assert_table_state(self.conn, uri, False, False, False)

        self.step_down()
        self.step_up()
        self.checkpoint_covering_epoch(20, 7)

        for uri, rows in tables:
            self.assert_table_state(self.conn, uri, True, True, True)
            self.assertEqual(self.read_kvs_at(uri, 8), rows)

    def test_window_create_then_drop(self):
        """
        A table created and dropped entirely inside the window never existed for any checkpoint, so
        the queued create and remove cancel out instead of tripping the violation check.
        """
        self.enter_window()

        uri, _ = self.create_with_rows('window_drop', 6)
        self.publish_if_epochs(uri, 20)
        self.dropUntilSuccess(self.session, uri)
        self.publish_if_epochs(uri, 20)

        self.complete_step_down(self.cutoff)
        self.step_up()
        self.checkpoint_covering_epoch(20, 7)

        self.assertFalse(self.uri_in_shared_metadata(self.conn, uri))
        self.assertEqual(self.local_metadata_keys(self.conn, uri), [])

    def test_pre_timestamp_reader_tolerates_window_create(self):
        """
        A read transaction that began before the step-down timestamp was set must tolerate a table
        created after it, whose stable constituent does not exist, instead of failing on the open.
        """
        self.setup_world()
        reader = self.conn.open_session('')
        reader.begin_transaction()

        self.set_step_down_ts(self.cutoff)
        uri, rows = self.create_with_rows('window_reader', 6)
        self.assertFalse(self.stable_constituent_exists(self.conn, uri))

        # The pre-timestamp snapshot predates the rows, so it reads an empty table rather than
        # failing on the missing constituent.
        cursor = reader.open_cursor(uri)
        cursor.set_key('k1')
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()
        reader.rollback_transaction()
        reader.close()

        # A reader that began after the timestamp sees the rows through the ingest constituent.
        self.assertEqual(self.read_kvs_at(uri, 7), rows)
        self.complete_step_down(self.cutoff)

    def test_create_racing_step_down_timestamp(self):
        """
        The step-down timestamp is published under the schema lock that creates hold, so a create
        racing it lands wholly on one side of the cutoff: either it built the stable constituent
        or it did not. A half-built table would show up as a create failure or as a table whose
        ingest constituent is missing.
        """
        self.setup_world()
        uris = [self.uri(f'race{i}') for i in range(30)]
        errors = []

        def create_tables():
            session = self.conn.open_session('')
            try:
                for uri in uris:
                    session.create(uri, self.table_config)
            except Exception as e:
                errors.append(e)
            finally:
                session.close()

        thread = threading.Thread(target=create_tables)
        thread.start()
        try:
            self.set_step_down_ts(self.cutoff)
        except Exception as e:
            errors.append(e)
        thread.join()
        self.assertEqual(errors, [])

        for uri in uris:
            self.assertTrue(self.uri_in_local_metadata(self.conn, uri))
        self.assert_no_unexpected_tables(self.conn, uris)
        self.complete_step_down(self.cutoff)
