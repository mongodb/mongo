"""Test hook that periodically promotes and demotes a replica set to/from a config server."""

import os.path
import random
import threading
import time

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.fixtures import replicaset
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


class ContinuousTransition(interface.Hook):
    """Regularly connect to replica sets and reconfigure it."""

    DESCRIPTION = "Continuous transition from a replica sets to config server replica sets, and viceversa at random time intervals"

    IS_BACKGROUND = True

    # The hook stops the fixture partially during its execution.
    STOPS_FIXTURE = True

    def __init__(
        self,
        hook_logger,
        fixture,
        transition_interval_min_ms=1000,
        transition_interval_max_ms=8000,
        is_fsm_workload=False,
        auth_options=None,
    ):
        """Initialize the ContinuousTransition.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: replica set fixture.
            transition_interval_min_ms: the minimum number of milliseconds between transitions.
            transition_interval_max_ms: the maximum number of milliseconds between transitions.
            is_fsm_workload: Whether the hook is running as an FSM workload is executing
            auth_options: dictionary of auth options.
        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousTransition.DESCRIPTION)

        self._fixture = fixture
        self._rs_fixtures = []
        self._transition_interval_min_secs = float(transition_interval_min_ms) / 1000
        self._transition_interval_max_secs = float(transition_interval_max_ms) / 1000

        self._transition_thread = None

        self._auth_options = auth_options

        # The action file names need to match the same construction as found in
        # jstests/concurrency/fsm_libs/resmoke_runner.js.
        dbpath_prefix = fixture.get_dbpath_prefix()

        # When running an FSM workload, we use the file-based lifecycle protocol
        # in which a file is used as a form of communication between the hook and
        # the FSM workload to decided when the hook is allowed to run.
        if is_fsm_workload:
            # Each hook uses a unique set of action files - the uniqueness is brought
            # about by using the hook's name as a suffix.
            self.__action_files = lifecycle_interface.ActionFiles._make(
                [
                    os.path.join(dbpath_prefix, field + "_" + self.__class__.__name__)
                    for field in lifecycle_interface.ActionFiles._fields
                ]
            )
        else:
            self.__action_files = None

    def before_suite(self, test_report):
        """Before suite."""
        if not self._rs_fixtures:
            for cluster in self._fixture.get_testable_clusters():
                self._add_fixture(cluster)

        if self.__action_files is not None:
            lifecycle = lifecycle_interface.FileBasedThreadLifecycle(self.__action_files)
        else:
            lifecycle = lifecycle_interface.FlagBasedThreadLifecycle()

        self._transition_thread = _TransitionThread(
            self.logger,
            self._rs_fixtures,
            self._transition_interval_min_secs,
            self._transition_interval_max_secs,
            lifecycle,
            self._fixture,
            self._auth_options,
        )
        self.logger.info("Starting the transition thread.")
        self._transition_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the transition thread.")
        self._transition_thread.stop()
        self.logger.info("Transition thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the transition thread.")
        self._transition_thread.pause()
        self._transition_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the transition thread.")
        self._transition_thread.pause()
        self.logger.info("Paused the transition thread.")

    def _add_fixture(self, fixture):
        if not fixture.all_nodes_electable:
            raise ValueError(
                "The replica sets that are the target of the ContinuousTransition hook must have"
                " the 'all_nodes_electable' option set."
            )
        if not isinstance(fixture, replicaset.ReplicaSetFixture):
            raise ValueError(
                "Transition to and from CSRS hook cannot be specified on any fixture other than a ReplicaSetFixture."
            )
        self._rs_fixtures.append(fixture)


class _TransitionThread(threading.Thread):
    def __init__(
        self,
        logger,
        rs_fixtures,
        transition_interval_min_secs,
        transition_interval_max_secs,
        stepdown_lifecycle,
        fixture,
        auth_options=None,
    ):
        """Initialize _TransitionThread."""
        threading.Thread.__init__(self, name="TransitionThread")
        self.daemon = True
        self.logger = logger
        self._rs_fixtures = rs_fixtures
        self._transition_interval_min_secs = transition_interval_min_secs
        self._transition_interval_max_secs = transition_interval_max_secs
        self._current_states = [0 for _rs_fixture in self._rs_fixtures]
        self.__lifecycle = stepdown_lifecycle
        self._fixture = fixture
        self._auth_options = auth_options

        self._last_exec = time.time()
        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is not performing stepdowns.
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()

        # Helpers to allow us to transition to and from valid states at random. The states
        # correspond to different combinations of startup flags and replica set configurations.
        self.states = [
            "REPLICA_SET",
            "CONFIG_SERVER_MAINTENANCE_MODE",
            "CONFIG_SERVER_MAINENANCE_MODE_RECONFIGURED",
            "CONFIG_SERVER",
            "REPLICA_SET_MAINENANCE_MODE",
            "REPLICA_SET_MAINTENANCE_MODE_RECONFIGURED",
        ]

        self.forward_transitions = {
            "REPLICA_SET": self._restart_as_csrs_maintenance,
            "CONFIG_SERVER_MAINTENANCE_MODE": self._reconfigure_as_csrs,
            "CONFIG_SERVER_MAINENANCE_MODE_RECONFIGURED": self._restart_as_csrs,
            "CONFIG_SERVER": self._restart_as_rs_maintenance,
            "REPLICA_SET_MAINENANCE_MODE": self._reconfigure_as_rs,
            "REPLICA_SET_MAINTENANCE_MODE_RECONFIGURED": self._restart_as_rs,
        }

        self.backward_transitions = {
            "CONFIG_SERVER": self._restart_as_csrs_maintenance,
            "CONFIG_SERVER_MAINENANCE_MODE_RECONFIGURED": self._reconfigure_as_rs,
            "CONFIG_SERVER_MAINTENANCE_MODE": self._restart_as_rs,
            "REPLICA_SET": self._restart_as_rs_maintenance,
            "REPLICA_SET_MAINENANCE_MODE": self._restart_as_csrs,
            "REPLICA_SET_MAINTENANCE_MODE_RECONFIGURED": self._reconfigure_as_csrs,
        }

    def run(self):
        """Execute the thread."""
        if not self._rs_fixtures:
            self.logger.warning("No replica set on which to run transitions.")
            return
        try:
            while True:
                self._is_idle_evt.set()

                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    break

                self._is_idle_evt.clear()

                for index, _rs_fixture in enumerate(self._rs_fixtures):
                    now = time.time()
                    current_state = self._current_states[index]
                    new_state, forward = self._get_next_state(current_state)
                    self.logger.info(
                        "Beginning transitioning replica set '%s' from state '%s' to state '%s'",
                        _rs_fixture.replset_name,
                        self.states[current_state],
                        self.states[new_state],
                    )
                    self._transition_states(_rs_fixture, current_state, forward)
                    self._current_states[index] = new_state
                    self.logger.info(
                        "Finished transitioning replica set '%s' from state '%s' to state '%s' in %0d ms",
                        _rs_fixture.replset_name,
                        self.states[current_state],
                        self.states[new_state],
                        (time.time() - now) * 1000,
                    )

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    self.__lifecycle.send_idle_acknowledgement()
                    continue

                pause_time = random.uniform(
                    self._transition_interval_min_secs,
                    self._transition_interval_max_secs,
                )
                self.__lifecycle.wait_for_action_interval(pause_time)
        except Exception:
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception("Transition Thread threw exception")
            # The event should be signaled whenever the thread is not performing stepdowns.
            self._is_idle_evt.set()

    def stop(self):
        """Stop the thread."""
        self.__lifecycle.stop()
        self._is_stopped_evt.set()
        # Unpause to allow the thread to finish.
        self.resume()
        self.join()

    def pause(self):
        """Pause the thread."""
        self.__lifecycle.mark_test_finished()

        # Wait until we are no longer executing stepdowns.
        self._is_idle_evt.wait()
        # Check if the thread is alive in case it has thrown an exception while running.
        self._check_thread()
        # Wait until the replica sets have a primary.
        self._await_primary()

        # Check that the fixture is still running
        for rs_fixture in self._rs_fixtures:
            if not rs_fixture.is_running():
                raise errors.ServerFailure(
                    "ReplicaSetFixture with pids {} expected to be running in"
                    " ContinuousTransition, but wasn't.".format(rs_fixture.pids())
                )

    def resume(self):
        """Resume the thread."""
        self.__lifecycle.mark_test_started()

    def _check_thread(self):
        if not self.is_alive():
            msg = "The transition thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _await_primary(self):
        for rs_fixture in self._rs_fixtures:
            rs_fixture.get_primary()

    def _get_next_state(self, current_state):
        # 25% chance to move backwards, 75% chance to move forwards
        transition_forward = random.randint(1, 4) > 1
        if transition_forward:
            return ((current_state + 1) % len(self.states), True)
        else:
            if current_state == 0:
                return (0, False)
            else:
                return (current_state - 1, False)

    def _transition_states(self, _rs_fixture, current_state, forward):
        if forward:
            self.forward_transitions[self.states[current_state]](_rs_fixture)
        else:
            self.backward_transitions[self.states[current_state]](_rs_fixture)

    def _restart_node(self, node, rs_fixture, enable_maintenance_mode, is_config_server):
        self.logger.info(
            "Waiting for mongod on port %d of replica set '%s' to stop and restart.",
            node.port,
            rs_fixture.replset_name,
        )
        node.mongod.stop(mode=fixture_interface.TeardownMode.TERMINATE)
        temporary_options = {}
        if is_config_server:
            temporary_options["configsvr"] = ""
        if enable_maintenance_mode:
            temporary_options["replicaSetConfigShardMaintenanceMode"] = ""
        rs_fixture.restart_node(node, temporary_options)
        self.logger.info(fixture_interface.create_fixture_table(rs_fixture))
        self.logger.info(
            "Restarted mongod on port %d of replica set '%s'.",
            node.port,
            rs_fixture.replset_name,
        )

    def _stepup_node_with_timeout(self, chosen, rs_fixture):
        retry_time_secs = 5 * 60  # Same timeout as in replicaset.py
        retry_start_time = time.time()

        while True:
            if rs_fixture.stepup_node(chosen, self._auth_options):
                break

            if time.time() - retry_start_time > retry_time_secs:
                raise errors.ServerFailure(
                    "Failed to step up a secondary in replica set '{}'.".format(
                        rs_fixture.replset_name
                    )
                )

    def _rolling_restart_nodes_demotion(
        self, rs_fixture, enable_maintenance_mode, is_config_server
    ):
        # To prevent write concern issues with mismatched node roles, first restart one secondary
        # then step up that secondary before continuing to restart all other secondaries.
        rs_name = rs_fixture.replset_name
        self.logger.info(
            "Restarting replica set '%s' with arguments: --configvr? '%s' and --replicaSetConfigShardMaintenanceMode? '%s'.",
            rs_name,
            is_config_server,
            enable_maintenance_mode,
        )

        old_primary = rs_fixture.get_primary()
        secondaries = rs_fixture.get_secondaries()
        first_secondary = secondaries[0]
        self._restart_node(first_secondary, rs_fixture, enable_maintenance_mode, is_config_server)
        self.logger.info(
            "Chose secondary on port %d of replica set '%s' for step up attempt.",
            first_secondary.port,
            rs_fixture.replset_name,
        )
        self._stepup_node_with_timeout(first_secondary, rs_fixture)
        new_secondaries = secondaries[1:]
        for node in new_secondaries:
            self._restart_node(node, rs_fixture, enable_maintenance_mode, is_config_server)
        self.logger.info("Finished restarting secondaries of replica set '%s'.", rs_name)
        self._restart_node(old_primary, rs_fixture, enable_maintenance_mode, is_config_server)
        self.logger.info("Finished rolling restart of replica set '%s'.", rs_name)

    def _rolling_restart_nodes_promotion(
        self, rs_fixture, enable_maintenance_mode, is_config_server
    ):
        # We are doing a rolling restart of the replica set members. This implies to restart
        # first all the secondaries, then choose randomly a secondary for stepping up, and
        # finally restart the old primary.
        def step_up_secondary():
            while secondaries:
                chosen = random.choice(secondaries)
                self.logger.info(
                    "Chose secondary on port %d of replica set '%s' for step up attempt.",
                    chosen.port,
                    rs_fixture.replset_name,
                )
                if not rs_fixture.stepup_node(chosen, self._auth_options):
                    self.logger.info(
                        "Attempt to step up secondary on port %d of replica set '%s' failed.",
                        chosen.port,
                        rs_fixture.replset_name,
                    )
                    secondaries.remove(chosen)
                else:
                    self.logger.info(
                        "Successfully stepped up the secondary on port %d of replica set '%s'.",
                        chosen.port,
                        rs_fixture.replset_name,
                    )
                    return chosen
            return None

        rs_name = rs_fixture.replset_name
        self.logger.info(
            "Restarting replica set '%s' with arguments: --configvr? '%s' and --replicaSetConfigShardMaintenanceMode? '%s'.",
            rs_name,
            is_config_server,
            enable_maintenance_mode,
        )

        old_primary = rs_fixture.get_primary()
        secondaries = rs_fixture.get_secondaries()
        if secondaries:
            for node in secondaries:
                self._restart_node(node, rs_fixture, enable_maintenance_mode, is_config_server)
            self.logger.info("Finished restarting secondaries of replica set '%s'.", rs_name)
            new_primary = step_up_secondary()
            if new_primary is None:
                raise errors.ServerFailure(
                    "Failed to step up a secondary in replica set '{}'.".format(rs_name)
                )
        self._restart_node(old_primary, rs_fixture, enable_maintenance_mode, is_config_server)
        self.logger.info("Finished rolling restart of replica set '%s'.", rs_name)

    def _restart_as_csrs_maintenance(self, rs_fixture):
        self._rolling_restart_nodes_promotion(
            rs_fixture, enable_maintenance_mode=True, is_config_server=True
        )

    def _reconfigure_as_csrs(self, rs_fixture):
        rs_name = rs_fixture.replset_name
        self.logger.info("Reconfiguring replica set '%s' to contain 'configsvr:true'.", rs_name)
        client = rs_fixture.get_primary().mongo_client()
        config = client.admin.command("replSetGetConfig")["config"]
        config["configsvr"] = True
        config["version"] = config["version"] + 1
        client.admin.command({"replSetReconfig": config})
        rs_fixture.await_last_op_committed()
        self.logger.info(
            "Successfully reconfigured primary on port %d of replica set '%s'.",
            rs_fixture.get_primary().port,
            rs_name,
        )

    def _restart_as_csrs(self, rs_fixture):
        self._rolling_restart_nodes_promotion(
            rs_fixture, enable_maintenance_mode=False, is_config_server=True
        )

    def _restart_as_rs_maintenance(self, rs_fixture):
        self._rolling_restart_nodes_demotion(
            rs_fixture, enable_maintenance_mode=True, is_config_server=False
        )

    def _reconfigure_as_rs(self, rs_fixture):
        rs_name = rs_fixture.replset_name
        self.logger.info("Reconfiguring replica set '%s' to not contain 'configsvr:true'.", rs_name)
        client = rs_fixture.get_primary().mongo_client()
        config = client.admin.command("replSetGetConfig")["config"]
        del config["configsvr"]
        config["version"] = config["version"] + 1
        client.admin.command({"replSetReconfig": config})
        rs_fixture.await_last_op_committed()
        self.logger.info(
            "Successfully reconfigured primary on port %d of replica set '%s'.",
            rs_fixture.get_primary().port,
            rs_name,
        )

    def _restart_as_rs(self, rs_fixture):
        self._rolling_restart_nodes_demotion(
            rs_fixture, enable_maintenance_mode=False, is_config_server=False
        )
