"""Unit tests for buildscripts/resmokelib/testing/hooks/stepdown.py."""

import logging
import unittest

import mock

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface
from buildscripts.resmokelib.testing.hooks import stepdown as _stepdown


class TestStepdownThread(unittest.TestCase):
    @mock.patch("buildscripts.resmokelib.testing.fixtures.replicaset.ReplicaSetFixture")
    @mock.patch("buildscripts.resmokelib.testing.fixtures.shardedcluster.ShardedClusterFixture")
    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.stepdown._StepdownThread.is_alive",
        mock.Mock(return_value=True),
    )
    def test_pause_throws_error(self, shardcluster_fixture, rs_fixture):
        stepdown_thread = _stepdown._StepdownThread(
            logger=logging.getLogger("hook_logger"),
            mongos_fixtures=[shardcluster_fixture.mongos],
            rs_fixtures=[rs_fixture],
            stepdown_interval_secs=8,
            terminate=False,
            kill=False,
            randomize_kill=False,
            stepdown_lifecycle=lifecycle_interface.FlagBasedThreadLifecycle(),
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

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.stepdown.fixture_interface.build_hook_client"
    )
    def test_step_down_via_command_does_not_step_up_a_secondary(self, build_hook_client):
        old_primary = mock.MagicMock(port=27017)
        new_primary = mock.MagicMock(port=27018)
        secondary = mock.MagicMock(port=27019)

        rs_fixture = mock.MagicMock()
        rs_fixture.removeshard_teardown_marker = False
        rs_fixture.replset_name = "rs0"
        rs_fixture.AWAIT_REPL_TIMEOUT_MINS = 5
        rs_fixture.get_primary.side_effect = [old_primary, new_primary]
        rs_fixture.get_secondaries.return_value = [secondary]

        client = mock.MagicMock()
        build_hook_client.return_value = client

        stepdown_thread = _stepdown._StepdownThread(
            logger=logging.getLogger("hook_logger"),
            mongos_fixtures=[],
            rs_fixtures=[rs_fixture],
            stepdown_interval_secs=8,
            terminate=False,
            kill=False,
            randomize_kill=False,
            stepdown_lifecycle=lifecycle_interface.FlagBasedThreadLifecycle(),
            background_reconfig=False,
            fixture=rs_fixture,
            use_stepdown_command=True,
        )

        stepdown_thread._step_down(rs_fixture)

        client.admin.command.assert_any_call(
            {"replSetStepDown": stepdown_thread._stepdown_duration_secs}
        )
        client.admin.command.assert_any_call({"replSetFreeze": 0})
        rs_fixture.stepup_node.assert_not_called()

        key = "{}/{}".format(rs_fixture.replset_name, new_primary.get_internal_connection_string())
        self.assertEqual(stepdown_thread._step_up_stats[key], 1)


if __name__ == "__main__":
    unittest.main()
