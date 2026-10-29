"""Unit tests for the fsync-unlock-after-test hook."""

import unittest

import mock

from buildscripts.resmokelib.testing.hooks import fsync_unlock_after_test


class TestFsyncUnlockAfterTest(unittest.TestCase):
    def setUp(self):
        self.hook_logger = mock.Mock()
        self.hook = fsync_unlock_after_test.FsyncUnlockAfterTest(self.hook_logger, mock.Mock())
        self.client = mock.Mock()
        self.node = mock.Mock()
        self.node.mongo_client.return_value = self.client

    def test_does_not_unlock_when_fixture_is_not_locked(self):
        with mock.patch.object(self.hook, "_is_supported_node", return_value=False):
            self.assertFalse(self.hook._unlock_if_needed(self.node))
        self.node.mongo_client.assert_not_called()

    def test_does_not_unlock_when_node_is_not_locked(self):
        self.client.admin.command.return_value = {"fsyncLock": False}
        with mock.patch.object(self.hook, "_is_supported_node", return_value=True):
            self.assertFalse(self.hook._unlock_if_needed(self.node))
        self.client.admin.command.assert_called_once_with({"currentOp": 1})
        self.client.close.assert_called_once()

    def test_unlocks_a_single_fsync_lock(self):
        self.client.admin.command.side_effect = [
            {"fsyncLock": True},
            {"ok": 1, "lockCount": 0},
            {"fsyncLock": False},
        ]
        with mock.patch.object(self.hook, "_is_supported_node", return_value=True):
            self.assertTrue(self.hook._unlock_if_needed(self.node))
        self.assertEqual(
            self.client.admin.command.call_args_list,
            [
                mock.call({"currentOp": 1}),
                mock.call({"fsyncUnlock": 1}),
                mock.call({"currentOp": 1}),
            ],
        )
        self.client.close.assert_called_once()

    def test_unlocks_all_nested_fsync_locks(self):
        self.client.admin.command.side_effect = [
            {"fsyncLock": True},
            {"ok": 1, "lockCount": 2},
            {"ok": 1, "lockCount": 1},
            {"ok": 1, "lockCount": 0},
            {"fsyncLock": False},
        ]
        with mock.patch.object(self.hook, "_is_supported_node", return_value=True):
            self.assertTrue(self.hook._unlock_if_needed(self.node))
        self.assertEqual(
            self.client.admin.command.call_args_list,
            [
                mock.call({"currentOp": 1}),
                mock.call({"fsyncUnlock": 1}),
                mock.call({"fsyncUnlock": 1}),
                mock.call({"fsyncUnlock": 1}),
                mock.call({"currentOp": 1}),
            ],
        )
        self.client.close.assert_called_once()

    def test_raises_when_fsync_lock_remains_after_unlocking(self):
        self.client.admin.command.side_effect = [
            {"fsyncLock": True},
            {"ok": 1, "lockCount": 0},
            {"fsyncLock": True},
        ]
        with mock.patch.object(self.hook, "_is_supported_node", return_value=True):
            with self.assertRaisesRegex(Exception, "Failed to unlock fsync-locked fixture"):
                self.hook._unlock_if_needed(self.node)
        self.client.close.assert_called_once()

    def test_after_test_raises_when_detected_node_is_not_unlocked(self):
        cluster = mock.Mock()
        cluster._all_mongo_d_s_t.return_value = [self.node]
        self.hook.fixture.get_independent_clusters.return_value = [cluster]
        with (
            mock.patch.object(self.hook, "_is_supported_node", return_value=True),
            mock.patch.object(self.hook, "_is_fsync_locked", return_value=True),
            mock.patch.object(self.hook, "_unlock_if_needed", return_value=False),
        ):
            with self.assertRaisesRegex(Exception, "Failed to unlock 1 fsync-locked nodes"):
                self.hook.after_test(mock.Mock(), mock.Mock())
        self.client.close.assert_called_once()

    def test_after_test_unlocks_detected_node(self):
        cluster = mock.Mock()
        cluster._all_mongo_d_s_t.return_value = [self.node]
        self.hook.fixture.get_independent_clusters.return_value = [cluster]
        with (
            mock.patch.object(self.hook, "_is_supported_node", return_value=True),
            mock.patch.object(self.hook, "_is_fsync_locked", return_value=True),
            mock.patch.object(
                self.hook, "_unlock_if_needed", return_value=True
            ) as unlock_if_needed,
        ):
            self.hook.after_test(mock.Mock(), mock.Mock())
        self.client.close.assert_called_once()
        unlock_if_needed.assert_called_once_with(self.node)
        self.hook_logger.info.assert_any_call("Successfully unlocked %s fsync-locked nodes", 1)
