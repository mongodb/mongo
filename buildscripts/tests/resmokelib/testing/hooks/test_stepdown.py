"""Unit tests for buildscripts/resmokelib/testing/hooks/stepdown.py."""

import logging
import os
import unittest

import mock

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import stepdown as _stepdown

# pylint: disable=protected-access


def _get_threading_lock(test_case, MockCondition):  # pylint: disable=invalid-name
    # There doesn't seem to be a better way to get the arguments that were passed in to the
    # constructor. We need to call release() on the threading.Lock in order for other methods on the
    # lifecycle object to be able to acquire() it.
    test_case.assertEqual(1, len(MockCondition.call_args_list))
    lock = MockCondition.call_args[0][0]
    return lock


class TestStepdownThread(unittest.TestCase):
    @mock.patch("buildscripts.resmokelib.testing.fixtures.replicaset.ReplicaSetFixture")
    @mock.patch("buildscripts.resmokelib.testing.fixtures.shardedcluster.ShardedClusterFixture")
    @mock.patch("buildscripts.resmokelib.testing.hooks.stepdown._StepdownThread.is_alive",
                mock.Mock(return_value=True))
    def test_pause_throws_error(self, shardcluster_fixture, rs_fixture):
        stepdown_thread = _stepdown._StepdownThread(
            logger=logging.getLogger("hook_logger"),
            mongos_fixtures=[shardcluster_fixture.mongos],
            rs_fixtures=[rs_fixture],
            stepdown_interval_secs=8,
            terminate=False,
            kill=False,
            stepdown_lifecycle=_stepdown.FlagBasedStepdownLifecycle(),
            wait_for_mongos_retarget=False,
            background_reconfig=False,
            fixture=shardcluster_fixture,
        )

        # doesn't throw error when fixtures are running
        stepdown_thread.pause()

        # throws error when replica set fixture is not running
        rs_fixture.is_running.return_value = False
        try:
            with self.assertRaises(errors.ServerFailure):
                stepdown_thread.pause()
        finally:
            rs_fixture.is_running.return_value = True

        # throws error when MongoS fixture is not running
        shardcluster_fixture.mongos.is_running.return_value = False
        with self.assertRaises(errors.ServerFailure):
            stepdown_thread.pause()


class TestFlagBasedStepdownLifecycle(unittest.TestCase):
    def test_becomes_idle_after_test_finishes(self):
        lifecycle = _stepdown.FlagBasedStepdownLifecycle()
        lifecycle.mark_test_started()
        self.assertFalse(lifecycle.poll_for_idle_request())
        lifecycle.mark_test_finished()
        self.assertTrue(lifecycle.poll_for_idle_request())

    def test_stepdown_permitted_after_test_starts(self):
        lifecycle = _stepdown.FlagBasedStepdownLifecycle()
        lifecycle.mark_test_started()
        self.assertTrue(lifecycle.wait_for_stepdown_permitted())

    @mock.patch("threading.Condition")
    def test_waiting_for_stepdown_permitted_is_interruptible(self, MockCondition):  # pylint: disable=invalid-name
        lifecycle = _stepdown.FlagBasedStepdownLifecycle()
        lifecycle.mark_test_started()
        lifecycle.mark_test_finished()

        def call_stop_while_waiting():
            lock = _get_threading_lock(self, MockCondition)
            lock.release()
            lifecycle.stop()
            lock.acquire()

        cond = MockCondition.return_value
        cond.wait.side_effect = call_stop_while_waiting

        self.assertFalse(lifecycle.wait_for_stepdown_permitted())
        self.assertTrue(cond.wait.called)


class TestFileBasedStepdownLifecycle(unittest.TestCase):

    STEPDOWN_FILES = _stepdown.StepdownFiles._make(_stepdown.StepdownFiles._fields)

    def test_still_idle_after_test_starts(self):
        lifecycle = _stepdown.FileBasedStepdownLifecycle(self.STEPDOWN_FILES)
        lifecycle.mark_test_started()
        self.assertFalse(lifecycle.poll_for_idle_request())

    @mock.patch("os.remove")
    def test_files_cleaned_up_after_test_finishes(self, mock_os_remove):
        lifecycle = _stepdown.FileBasedStepdownLifecycle(self.STEPDOWN_FILES)
        lifecycle.mark_test_started()

        lifecycle.mark_test_finished()
        mock_os_remove.assert_any_call("permitted")
        mock_os_remove.assert_any_call("idle_request")
        mock_os_remove.assert_any_call("idle_ack")

        mock_os_remove.reset_mock()
        mock_os_remove.side_effect = OSError("Pretend that the file wasn't found")

        lifecycle.mark_test_finished()
        mock_os_remove.assert_any_call("permitted")
        mock_os_remove.assert_any_call("idle_request")
        mock_os_remove.assert_any_call("idle_ack")

    @mock.patch("os.path")
    def test_stepdown_permitted_if_permitted_file_exists(self, mock_os_path):
        lifecycle = _stepdown.FileBasedStepdownLifecycle(self.STEPDOWN_FILES)
        lifecycle.mark_test_started()

        def mock_does_permitted_file_exists(filename):
            if filename == "permitted":
                return permitted_file_exists

            return os.path.isfile(filename)

        mock_os_path.isfile = mock_does_permitted_file_exists

        permitted_file_exists = True
        self.assertTrue(lifecycle.wait_for_stepdown_permitted())

    @mock.patch("threading.Condition")
    @mock.patch("os.path")
    def test_stepdown_waits_until_permitted_file_exists(self, mock_os_path, MockCondition):  # pylint: disable=invalid-name
        lifecycle = _stepdown.FileBasedStepdownLifecycle(self.STEPDOWN_FILES)
        lifecycle.mark_test_started()

        def mock_does_permitted_file_exists(filename):  # pylint: disable=inconsistent-return-statements
            if filename == "permitted":
                return permitted_file_exists

            self.fail("Mock called with unexpected filename: %s" % (filename, ))

        mock_os_path.isfile = mock_does_permitted_file_exists

        def create_permitted_file_while_waiting(_timeout):
            nonlocal permitted_file_exists
            permitted_file_exists = True

        cond = MockCondition.return_value
        cond.wait.side_effect = create_permitted_file_while_waiting

        permitted_file_exists = False
        self.assertTrue(lifecycle.wait_for_stepdown_permitted())
        self.assertTrue(cond.wait.called)

    @mock.patch("threading.Condition")
    @mock.patch("os.path")
    def test_waiting_for_stepdown_permitted_is_interruptible(self, mock_os_path, MockCondition):  # pylint: disable=invalid-name
        lifecycle = _stepdown.FileBasedStepdownLifecycle(self.STEPDOWN_FILES)
        lifecycle.mark_test_started()

        mock_os_path.isfile.return_value = False

        def call_stop_while_waiting(_timeout):
            lock = _get_threading_lock(self, MockCondition)
            lock.release()
            lifecycle.stop()
            lock.acquire()

        cond = MockCondition.return_value
        cond.wait.side_effect = call_stop_while_waiting

        self.assertFalse(lifecycle.wait_for_stepdown_permitted())
        self.assertTrue(cond.wait.called)
