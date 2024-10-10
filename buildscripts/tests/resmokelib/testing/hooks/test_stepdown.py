"""Unit tests for buildscripts/resmokelib/testing/hooks/stepdown.py."""

import logging
import unittest

import mock

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface
from buildscripts.resmokelib.testing.hooks import stepdown as _stepdown

# pylint: disable=protected-access


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
