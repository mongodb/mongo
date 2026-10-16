"""Unit tests for the ContinuousTransition hook."""
# TODO(SERVER-131909): Remove this test file once disaggregated replica sets have elegant stepdown.

import logging
import subprocess
import unittest

import mock

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.core import process
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import replicaset, standalone
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface
from buildscripts.resmokelib.testing.hooks import (
    replicaset_transition_to_and_from_csrs as continuous_transition,
)


def _make_node(port):
    node = mock.Mock(spec=standalone.MongoDFixture)
    node.port = port
    node.mongod = mock.Mock(spec=process.Process)
    return node


class TestTransitionThread(unittest.TestCase):
    def setUp(self):
        self.fixture = mock.Mock(spec=fixture_interface.Fixture)
        self.rs_fixture = mock.Mock(spec=replicaset.ReplicaSetFixture)
        self.rs_fixture.replset_name = "rs0"
        self.rs_fixture.get_node_info.return_value = []
        self.rs_fixture.requires_configsvr_replset_config.return_value = True
        self.thread = continuous_transition._TransitionThread(
            logger=logging.getLogger("hook_logger"),
            rs_fixtures=[self.rs_fixture],
            transition_interval_min_secs=1,
            transition_interval_max_secs=2,
            stepdown_lifecycle=lifecycle_interface.FlagBasedThreadLifecycle(),
            fixture=self.fixture,
        )

    def test_restart_node_passes_stop_timeout_to_fixture(self):
        node = _make_node(27018)
        stop_timeout_secs = 30

        self.thread._restart_node(
            node,
            self.rs_fixture,
            enable_maintenance_mode=False,
            is_config_server=False,
            stop_timeout_secs=stop_timeout_secs,
        )

        node.mongod.stop.assert_called_once_with(mode=fixture_interface.TeardownMode.TERMINATE)
        self.rs_fixture.restart_node.assert_called_once_with(node, {}, timeout=stop_timeout_secs)

    def test_restart_node_passes_config_server_and_maintenance_flags(self):
        node = _make_node(27018)

        self.thread._restart_node(
            node,
            self.rs_fixture,
            enable_maintenance_mode=True,
            is_config_server=True,
        )

        node.mongod.stop.assert_called_once_with(mode=fixture_interface.TeardownMode.TERMINATE)
        self.rs_fixture.restart_node.assert_called_once_with(
            node,
            {
                "configsvr": "",
                "replicaSetConfigShardMaintenanceMode": "",
            },
            timeout=None,
        )

    def test_standard_fixture_reconfigures_replica_set_config(self):
        primary = _make_node(27017)
        client = primary.mongo_client.return_value
        config_to_csrs = {"version": 1}
        config_to_rs = {"version": 2, "configsvr": True}
        client.admin.command.side_effect = [
            {"config": config_to_csrs},
            {},
            {"config": config_to_rs},
            {},
        ]
        self.rs_fixture.get_primary.return_value = primary

        self.thread._reconfigure_as_csrs(self.rs_fixture)
        self.thread._reconfigure_as_rs(self.rs_fixture)

        self.assertEqual(
            client.admin.command.call_args_list,
            [
                mock.call("replSetGetConfig"),
                mock.call({"replSetReconfig": {"version": 2, "configsvr": True}}),
                mock.call("replSetGetConfig"),
                mock.call({"replSetReconfig": {"version": 3}}),
            ],
        )
        self.assertEqual(self.rs_fixture.await_last_op_committed.call_count, 2)

    def test_fixture_with_startup_config_server_role_skips_legacy_reconfigs(self):
        self.rs_fixture.requires_configsvr_replset_config.return_value = False

        self.thread._reconfigure_as_csrs(self.rs_fixture)
        self.thread._reconfigure_as_rs(self.rs_fixture)

        self.rs_fixture.get_primary.assert_not_called()
        self.rs_fixture.await_last_op_committed.assert_not_called()

    def test_demotion_restarts_primary_before_stepup_when_required(self):
        old_primary = _make_node(27017)
        first_secondary = _make_node(27018)
        second_secondary = _make_node(27019)
        self.rs_fixture.get_primary.return_value = old_primary
        self.rs_fixture.get_secondaries.return_value = [first_secondary, second_secondary]
        self.rs_fixture.should_stop_primary_before_stepup.return_value = True
        self.thread._restart_node = mock.Mock()
        self.thread._stepup_node_with_timeout = mock.Mock()

        calls = mock.Mock()
        calls.attach_mock(self.thread._restart_node, "restart_node")
        calls.attach_mock(self.thread._stepup_node_with_timeout, "stepup")

        self.thread._rolling_restart_nodes_demotion(
            self.rs_fixture, enable_maintenance_mode=True, is_config_server=False
        )

        self.assertEqual(
            calls.mock_calls,
            [
                mock.call.restart_node(first_secondary, self.rs_fixture, True, False),
                mock.call.restart_node(
                    old_primary,
                    self.rs_fixture,
                    True,
                    False,
                    stop_timeout_secs=fixture_interface.Fixture.AWAIT_READY_TIMEOUT_SECS,
                ),
                mock.call.stepup(first_secondary, self.rs_fixture),
                mock.call.restart_node(second_secondary, self.rs_fixture, True, False),
            ],
        )

    def test_demotion_restarts_primary_after_stepup_when_not_required(self):
        old_primary = _make_node(27017)
        first_secondary = _make_node(27018)
        second_secondary = _make_node(27019)
        self.rs_fixture.get_primary.return_value = old_primary
        self.rs_fixture.get_secondaries.return_value = [first_secondary, second_secondary]
        self.rs_fixture.should_stop_primary_before_stepup.return_value = False
        self.thread._restart_node = mock.Mock()
        self.thread._stepup_node_with_timeout = mock.Mock()

        calls = mock.Mock()
        calls.attach_mock(self.thread._restart_node, "restart_node")
        calls.attach_mock(self.thread._stepup_node_with_timeout, "stepup")

        self.thread._rolling_restart_nodes_demotion(
            self.rs_fixture, enable_maintenance_mode=True, is_config_server=False
        )

        self.assertEqual(
            calls.mock_calls,
            [
                mock.call.restart_node(first_secondary, self.rs_fixture, True, False),
                mock.call.stepup(first_secondary, self.rs_fixture),
                mock.call.restart_node(second_secondary, self.rs_fixture, True, False),
                mock.call.restart_node(old_primary, self.rs_fixture, True, False),
            ],
        )

    @mock.patch(
        "buildscripts.resmokelib.testing.hooks.replicaset_transition_to_and_from_csrs.random.choice"
    )
    def test_promotion_stops_primary_before_stepup_when_required(self, random_choice):
        old_primary = _make_node(27017)
        first_secondary = _make_node(27018)
        second_secondary = _make_node(27019)
        random_choice.return_value = second_secondary
        self.rs_fixture.get_primary.return_value = old_primary
        self.rs_fixture.get_secondaries.return_value = [first_secondary, second_secondary]
        self.rs_fixture.should_stop_primary_before_stepup.return_value = True
        self.thread._restart_node = mock.Mock()
        self.thread._stepup_node_with_timeout = mock.Mock()

        calls = mock.Mock()
        calls.attach_mock(self.thread._restart_node, "restart_node")
        calls.attach_mock(self.thread._stepup_node_with_timeout, "stepup")

        self.thread._rolling_restart_nodes_promotion(
            self.rs_fixture, enable_maintenance_mode=False, is_config_server=True
        )

        self.assertEqual(
            calls.mock_calls,
            [
                mock.call.restart_node(first_secondary, self.rs_fixture, False, True),
                mock.call.restart_node(second_secondary, self.rs_fixture, False, True),
                mock.call.restart_node(
                    old_primary,
                    self.rs_fixture,
                    False,
                    True,
                    stop_timeout_secs=fixture_interface.Fixture.AWAIT_READY_TIMEOUT_SECS,
                ),
                mock.call.stepup(second_secondary, self.rs_fixture),
            ],
        )


class TestReplicaSetFixture(unittest.TestCase):
    def setUp(self):
        self.fixture = mock.Mock(spec=replicaset.ReplicaSetFixture)
        self.fixture.logger = mock.Mock()
        self.fixture.fixturelib = mock.Mock()
        self.fixture.fixturelib.ServerFailure = errors.ServerFailure
        self.fixture.replset_name = "rs0"
        self.node = _make_node(27017)
        self.node.preserve_dbpath = False

    @mock.patch(
        "buildscripts.resmokelib.testing.fixtures.replicaset.interface.create_fixture_table",
        return_value=None,
    )
    def test_restart_node_passes_timeout_to_mongod_wait(self, _create_fixture_table):
        self.node.mongod.wait.return_value = 0

        replicaset.ReplicaSetFixture.restart_node(
            self.fixture, self.node, timeout=fixture_interface.Fixture.AWAIT_READY_TIMEOUT_SECS
        )

        self.node.mongod.wait.assert_called_once_with(
            timeout=fixture_interface.Fixture.AWAIT_READY_TIMEOUT_SECS
        )

    def test_restart_node_raises_server_failure_when_wait_times_out(self):
        self.node.mongod.wait.side_effect = subprocess.TimeoutExpired(
            cmd="mongod", timeout=fixture_interface.Fixture.AWAIT_READY_TIMEOUT_SECS
        )

        with self.assertRaisesRegex(errors.ServerFailure, "Timed out"):
            replicaset.ReplicaSetFixture.restart_node(
                self.fixture,
                self.node,
                timeout=fixture_interface.Fixture.AWAIT_READY_TIMEOUT_SECS,
            )


if __name__ == "__main__":
    unittest.main()
