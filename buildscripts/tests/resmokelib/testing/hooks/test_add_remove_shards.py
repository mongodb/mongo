"""Unit tests for the stall detection and index-inconsistency recovery logic in
buildscripts/resmokelib/testing/hooks/add_remove_shards.py."""

import logging
import unittest
from unittest import mock

import pymongo.errors

from buildscripts.resmokelib.testing.hooks import add_remove_shards
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


def _make_thread():
    """Return a (_AddRemoveShardThread, mock_client) pair with all external I/O mocked."""
    mock_client = mock.MagicMock()
    # listShards is called during __init__ via _current_fixture_mode.
    mock_client.admin.command.return_value = {
        "shards": [{"_id": "shard-rs0"}, {"_id": "shard-rs1"}]
    }

    with mock.patch(
        "buildscripts.resmokelib.testing.fixtures.interface.build_client",
        return_value=mock_client,
    ):
        thread = add_remove_shards._AddRemoveShardThread(
            logger=logging.getLogger("test"),
            life_cycle=lifecycle_interface.FlagBasedThreadLifecycle(),
            fixture=mock.MagicMock(),
            auth_options=None,
            random_balancer_on=False,
            transition_configsvr=False,
            add_remove_random_shards=True,
            move_primary_comment=None,
            move_sessions_collection=False,
            transition_intervals=[10],
        )

    thread._client = mock_client
    return thread, mock_client


def _make_sharded_coll(namespace, num_chunks):
    """Build a minimal sharded-collection document as returned by _get_tracked_collections_on_shard."""
    return {
        "_id": namespace,
        "key": {"x": 1},
        "chunksOnRemovedShard": [{"_id": i} for i in range(num_chunks)],
    }


class TestHandleStalledShardedCollections(unittest.TestCase):
    def setUp(self):
        self.thread, _ = _make_thread()

    def _call(self, sharded_colls):
        self.thread._handle_stalled_sharded_collections(sharded_colls, "shard-rs0")

    def test_initial_call_sets_state_without_incrementing(self):
        self._call([_make_sharded_coll("db.coll", 2)])
        self.assertEqual(self.thread._sharded_colls_unchanged_rounds, 0)

    def test_counter_increments_when_state_is_unchanged(self):
        colls = [_make_sharded_coll("db.coll", 2)]
        self._call(colls)
        self._call(colls)
        self.assertEqual(self.thread._sharded_colls_unchanged_rounds, 1)

    def test_counter_resets_when_chunk_count_decreases(self):
        self._call([_make_sharded_coll("db.coll", 2)])
        self._call([_make_sharded_coll("db.coll", 2)])  # counter = 1
        self._call([_make_sharded_coll("db.coll", 1)])  # chunk count changed
        self.assertEqual(self.thread._sharded_colls_unchanged_rounds, 0)

    def test_counter_resets_when_collection_drains_completely(self):
        self._call([_make_sharded_coll("db.coll", 1)])
        self._call([_make_sharded_coll("db.coll", 1)])  # counter = 1
        self._call([])  # collection gone
        self.assertEqual(self.thread._sharded_colls_unchanged_rounds, 0)

    def test_counter_resets_when_new_collection_appears(self):
        self._call([_make_sharded_coll("db.coll", 1)])
        self._call([_make_sharded_coll("db.coll", 1)])  # counter = 1
        self._call([_make_sharded_coll("db.coll", 1), _make_sharded_coll("db.other", 1)])
        self.assertEqual(self.thread._sharded_colls_unchanged_rounds, 0)

    def test_check_not_triggered_before_threshold(self):
        self.thread._check_and_reshard_if_index_inconsistent = mock.MagicMock()
        colls = [_make_sharded_coll("db.coll", 1)]
        # 1 call sets state; 9 more give counter 1..9 — none are multiples of 10.
        for _ in range(10):
            self._call(colls)
        self.thread._check_and_reshard_if_index_inconsistent.assert_not_called()

    def test_check_triggered_at_threshold(self):
        self.thread._check_and_reshard_if_index_inconsistent = mock.MagicMock()
        colls = [_make_sharded_coll("db.coll", 1)]
        # 1 call sets state + 10 stalled rounds → counter hits 10.
        for _ in range(11):
            self._call(colls)
        self.thread._check_and_reshard_if_index_inconsistent.assert_called_once_with(
            "db.coll", {"x": 1}, "shard-rs0"
        )

    def test_check_triggered_again_at_next_multiple_of_threshold(self):
        self.thread._check_and_reshard_if_index_inconsistent = mock.MagicMock()
        colls = [_make_sharded_coll("db.coll", 1)]
        # 1 call sets state + 20 stalled rounds → triggered at counter 10 and 20.
        for _ in range(21):
            self._call(colls)
        self.assertEqual(self.thread._check_and_reshard_if_index_inconsistent.call_count, 2)

    def test_counter_stays_zero_when_no_collections_remain(self):
        # Prime the counter to a non-zero value, then drain all collections.
        colls = [_make_sharded_coll("db.coll", 1)]
        self._call(colls)
        self._call(colls)  # counter = 1
        self._call([])  # all drained — counter resets to 0
        # Subsequent calls with no collections must not increment the counter.
        for _ in range(15):
            self._call([])
        self.assertEqual(self.thread._sharded_colls_unchanged_rounds, 0)

    def test_check_triggered_for_each_stalled_collection(self):
        self.thread._check_and_reshard_if_index_inconsistent = mock.MagicMock()
        colls = [_make_sharded_coll("db.coll1", 1), _make_sharded_coll("db.coll2", 2)]
        for _ in range(11):
            self._call(colls)
        self.assertEqual(self.thread._check_and_reshard_if_index_inconsistent.call_count, 2)

    def test_empty_collections_on_first_call_leaves_counter_at_zero(self):
        # _prev_sharded_coll_state starts as None; the first call with no collections
        # (shard already fully drained) must set prev to an empty frozenset without
        # incrementing the counter or triggering any check.
        self.thread._check_and_reshard_if_index_inconsistent = mock.MagicMock()
        self._call([])
        self.assertEqual(self.thread._sharded_colls_unchanged_rounds, 0)
        self.assertEqual(self.thread._prev_sharded_coll_state, frozenset())
        self.thread._check_and_reshard_if_index_inconsistent.assert_not_called()

    def test_empty_first_call_then_nonempty_starts_fresh_tracking(self):
        # If the shard is empty on the very first poll and collections appear later,
        # the counter must start from 0 rather than treating the transition as a stall.
        self.thread._check_and_reshard_if_index_inconsistent = mock.MagicMock()
        self._call([])
        self._call([_make_sharded_coll("db.coll", 1)])
        self.assertEqual(self.thread._sharded_colls_unchanged_rounds, 0)
        self.thread._check_and_reshard_if_index_inconsistent.assert_not_called()


class TestCheckAndReshardIfIndexInconsistent(unittest.TestCase):
    def setUp(self):
        self.thread, self.mock_client = _make_thread()
        self.thread._reshard_collection_off_shard = mock.MagicMock()

    def _set_cmc_response(self, inconsistencies):
        self.mock_client.__getitem__.return_value.command.return_value = {
            "cursor": {"firstBatch": inconsistencies, "id": 0}
        }

    def test_reshards_when_index_inconsistency_found(self):
        self._set_cmc_response([{"type": "InconsistentIndex"}])
        self.thread._check_and_reshard_if_index_inconsistent("db.coll", {"x": 1}, "shard-rs0")
        self.thread._reshard_collection_off_shard.assert_called_once_with(
            "db.coll", {"x": 1}, "shard-rs0"
        )

    def test_reshards_when_index_inconsistency_in_later_batch(self):
        # Simulate the inconsistency arriving in a getMore batch rather than firstBatch,
        # which is the scenario the getMore loop in the hook is meant to handle.
        db_mock = self.mock_client.__getitem__.return_value
        db_mock.command.side_effect = [
            {"cursor": {"firstBatch": [], "id": 99}},
            {"cursor": {"nextBatch": [{"type": "InconsistentIndex"}], "id": 0}},
        ]
        self.thread._check_and_reshard_if_index_inconsistent("db.coll", {"x": 1}, "shard-rs0")
        self.thread._reshard_collection_off_shard.assert_called_once_with(
            "db.coll", {"x": 1}, "shard-rs0"
        )

    def test_no_reshard_when_no_inconsistencies(self):
        self._set_cmc_response([])
        self.thread._check_and_reshard_if_index_inconsistent("db.coll", {"x": 1}, "shard-rs0")
        self.thread._reshard_collection_off_shard.assert_not_called()

    def test_no_reshard_for_unrelated_inconsistency_type(self):
        self._set_cmc_response([{"type": "MisplacedCollection"}])
        self.thread._check_and_reshard_if_index_inconsistent("db.coll", {"x": 1}, "shard-rs0")
        self.thread._reshard_collection_off_shard.assert_not_called()

    def test_no_reshard_on_cmc_operation_failure(self):
        self.mock_client.__getitem__.return_value.command.side_effect = (
            pymongo.errors.OperationFailure("metadata check failed")
        )
        self.thread._check_and_reshard_if_index_inconsistent("db.coll", {"x": 1}, "shard-rs0")
        self.thread._reshard_collection_off_shard.assert_not_called()

    def test_no_reshard_on_cmc_connection_failure(self):
        self.mock_client.__getitem__.return_value.command.side_effect = (
            pymongo.errors.AutoReconnect("connection reset — stepdown")
        )
        self.thread._check_and_reshard_if_index_inconsistent("db.coll", {"x": 1}, "shard-rs0")
        self.thread._reshard_collection_off_shard.assert_not_called()

    def test_no_reshard_on_getmore_operation_failure(self):
        db_mock = self.mock_client.__getitem__.return_value
        db_mock.command.side_effect = [
            {"cursor": {"firstBatch": [], "id": 99}},
            pymongo.errors.OperationFailure("cursor killed"),
        ]
        self.thread._check_and_reshard_if_index_inconsistent("db.coll", {"x": 1}, "shard-rs0")
        self.thread._reshard_collection_off_shard.assert_not_called()

    def test_no_reshard_on_getmore_connection_failure(self):
        db_mock = self.mock_client.__getitem__.return_value
        db_mock.command.side_effect = [
            {"cursor": {"firstBatch": [], "id": 99}},
            pymongo.errors.AutoReconnect("connection reset — stepdown"),
        ]
        self.thread._check_and_reshard_if_index_inconsistent("db.coll", {"x": 1}, "shard-rs0")
        self.thread._reshard_collection_off_shard.assert_not_called()


class TestReshardCollectionOffShard(unittest.TestCase):
    def setUp(self):
        self.thread, self.mock_client = _make_thread()
        self.mock_client.admin.command.reset_mock()

    def _expected_cmd(self, namespace, shard_key):
        # The server's split policy (SamplingBasedSplitPolicy for range keys,
        # SplitPointsBasedSplitPolicy for hashed-prefix keys) calls
        # getAllNonDrainingShardIdsShuffled(), which excludes draining shards. So the hook
        # omits shardDistribution for all key types and lets the server choose recipients.
        return {"reshardCollection": namespace, "key": shard_key, "forceRedistribution": True}

    def test_issues_correct_reshard_command_for_range_key(self):
        self.thread._reshard_collection_off_shard("db.coll", {"x": 1}, "shard-rs0")
        self.mock_client.admin.command.assert_called_once_with(
            self._expected_cmd("db.coll", {"x": 1})
        )

    def test_issues_correct_reshard_command_for_hashed_key(self):
        self.thread._reshard_collection_off_shard("db.coll", {"x": "hashed"}, "shard-rs0")
        self.mock_client.admin.command.assert_called_once_with(
            self._expected_cmd("db.coll", {"x": "hashed"})
        )

    def test_issues_correct_reshard_command_for_compound_key(self):
        self.thread._reshard_collection_off_shard("db.coll", {"a": 1, "b": 1}, "shard-rs0")
        self.mock_client.admin.command.assert_called_once_with(
            self._expected_cmd("db.coll", {"a": 1, "b": 1})
        )

    def test_ignores_reshard_collection_aborted(self):
        self.mock_client.admin.command.side_effect = pymongo.errors.OperationFailure(
            "aborted",
            code=add_remove_shards._AddRemoveShardThread._RESHARD_COLLECTION_ABORTED,
        )
        self.thread._reshard_collection_off_shard("db.coll", {"x": 1}, "shard-rs0")

    def test_ignores_reshard_collection_in_progress(self):
        self.mock_client.admin.command.side_effect = pymongo.errors.OperationFailure(
            "in progress",
            code=add_remove_shards._AddRemoveShardThread._RESHARD_COLLECTION_IN_PROGRESS,
        )
        self.thread._reshard_collection_off_shard("db.coll", {"x": 1}, "shard-rs0")

    def test_raises_on_unexpected_error(self):
        self.mock_client.admin.command.side_effect = pymongo.errors.OperationFailure(
            "unexpected", code=99999
        )
        with self.assertRaises(pymongo.errors.OperationFailure):
            self.thread._reshard_collection_off_shard("db.coll", {"x": 1}, "shard-rs0")


if __name__ == "__main__":
    unittest.main()
