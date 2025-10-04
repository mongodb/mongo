"""Unit tests for buildscripts/resmokelib/testing/hooks/lifecycle.py."""

import os
import unittest

import mock

from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


def _get_threading_lock(test_case, MockCondition):
    # There doesn't seem to be a better way to get the arguments that were passed in to the
    # constructor. We need to call release() on the threading.Lock in order for other methods on the
    # lifecycle object to be able to acquire() it.
    test_case.assertEqual(1, len(MockCondition.call_args_list))
    lock = MockCondition.call_args[0][0]
    return lock


class TestFlagBasedThreadLifecycle(unittest.TestCase):
    def test_becomes_idle_after_test_finishes(self):
        lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()
        lifecycle.mark_test_started()
        self.assertFalse(lifecycle.poll_for_idle_request())
        lifecycle.mark_test_finished()
        self.assertTrue(lifecycle.poll_for_idle_request())

    def test_action_permitted_after_test_starts(self):
        lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()
        lifecycle.mark_test_started()
        self.assertTrue(lifecycle.wait_for_action_permitted())

    @mock.patch("threading.Condition")
    def test_waiting_for_action_permitted_is_interruptible(self, MockCondition):
        lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()
        lifecycle.mark_test_started()
        lifecycle.mark_test_finished()

        def call_stop_while_waiting():
            lock = _get_threading_lock(self, MockCondition)
            lock.release()
            lifecycle.stop()
            lock.acquire()

        cond = MockCondition.return_value
        cond.wait.side_effect = call_stop_while_waiting

        self.assertFalse(lifecycle.wait_for_action_permitted())
        self.assertTrue(cond.wait.called)


class TestFileBasedThreadLifecycle(unittest.TestCase):
    ACTION_FILES = lifecycle_interface.ActionFiles._make(lifecycle_interface.ActionFiles._fields)

    def test_still_idle_after_test_starts(self):
        lifecycle = lifecycle_interface.FileBasedThreadLifecycle(self.ACTION_FILES)
        lifecycle.mark_test_started()
        self.assertFalse(lifecycle.poll_for_idle_request())

    @mock.patch("os.remove")
    def test_files_cleaned_up_after_test_finishes(self, mock_os_remove):
        lifecycle = lifecycle_interface.FileBasedThreadLifecycle(self.ACTION_FILES)
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
    def test_action_permitted_if_permitted_file_exists(self, mock_os_path):
        lifecycle = lifecycle_interface.FileBasedThreadLifecycle(self.ACTION_FILES)
        lifecycle.mark_test_started()

        def mock_does_permitted_file_exists(filename):
            if filename == "permitted":
                return permitted_file_exists

            return os.path.isfile(filename)

        mock_os_path.isfile = mock_does_permitted_file_exists

        permitted_file_exists = True
        self.assertTrue(lifecycle.wait_for_action_permitted())

    @mock.patch("threading.Condition")
    @mock.patch("os.path")
    def test_thread_waits_until_permitted_file_exists(self, mock_os_path, MockCondition):
        lifecycle = lifecycle_interface.FileBasedThreadLifecycle(self.ACTION_FILES)
        lifecycle.mark_test_started()

        def mock_does_permitted_file_exists(filename):
            if filename == "permitted":
                return permitted_file_exists

            self.fail("Mock called with unexpected filename: %s" % (filename,))

        mock_os_path.isfile = mock_does_permitted_file_exists

        def create_permitted_file_while_waiting(_timeout):
            nonlocal permitted_file_exists
            permitted_file_exists = True

        cond = MockCondition.return_value
        cond.wait.side_effect = create_permitted_file_while_waiting

        permitted_file_exists = False
        self.assertTrue(lifecycle.wait_for_action_permitted())
        self.assertTrue(cond.wait.called)

    @mock.patch("threading.Condition")
    @mock.patch("os.path")
    def test_waiting_for_action_permitted_is_interruptible(self, mock_os_path, MockCondition):
        lifecycle = lifecycle_interface.FileBasedThreadLifecycle(self.ACTION_FILES)
        lifecycle.mark_test_started()

        mock_os_path.isfile.return_value = False

        def call_stop_while_waiting(_timeout):
            lock = _get_threading_lock(self, MockCondition)
            lock.release()
            lifecycle.stop()
            lock.acquire()

        cond = MockCondition.return_value
        cond.wait.side_effect = call_stop_while_waiting

        self.assertFalse(lifecycle.wait_for_action_permitted())
        self.assertTrue(cond.wait.called)
