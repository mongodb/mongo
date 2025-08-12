"""Test hook that periodically makes a secondary enter maintenance mode. This hook should be used in combination with the `linear_chain` option in ReplicaSetFixture."""

import os.path
import random
import threading
import time

import pymongo

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


class ContinuousMaintenance(interface.Hook):
    """Regularly connect to replica sets and send a replSetMaintenance command."""

    DESCRIPTION = (
        "Continuous maintenance (causes a secondary node to enter maintenance mode at regular" " intervals)"
    )

    IS_BACKGROUND = True

    # The hook stops the fixture partially during its execution.
    STOPS_FIXTURE = True

    def __init__(
        self,
        hook_logger,
        fixture,
        maintenance_interval_ms=5000,
        is_fsm_workload=False,
        auth_options=None,
    ):
        """Initialize the ContinuousMaintenance.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture replica sets.
            maintenance_interval_ms: the number of milliseconds between maintenances.
            is_fsm_workload: Whether the hook is running as an FSM workload is executing
            auth_options: dictionary of auth options.
        """
        interface.Hook.__init__(self, hook_logger, fixture, ContinuousMaintenance.DESCRIPTION)

        self._fixture = fixture
        self._maintenance_interval_secs = float(maintenance_interval_ms) / 1000

        self._rs_fixtures = []
        self._maintenance_thread = None

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

        self._maintenance_thread = _MaintenanceThread(
            self.logger,
            self._rs_fixtures,
            self._maintenance_interval_secs,
            lifecycle,
            self._fixture,
            self._auth_options,
        )
        self.logger.info("Starting the maintenance thread.")
        self._maintenance_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the maintenance thread.")
        self._maintenance_thread._none_maintenance_mode()
        self._maintenance_thread.stop()
        self.logger.info("Maintenance thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the maintenance thread.")
        self._maintenance_thread._none_maintenance_mode()
        self._maintenance_thread.pause()
        self._maintenance_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the maintenance thread.")
        self._maintenance_thread._none_maintenance_mode()
        self._maintenance_thread.pause()
        self.logger.info("Paused the maintenance thread.")

    def _add_fixture(self, fixture):
        self._rs_fixtures.append(fixture)


class _MaintenanceThread(threading.Thread):
    def __init__(
        self,
        logger,
        rs_fixtures,
        maintenance_interval_secs,
        maintenance_lifecycle,
        fixture,
        auth_options=None,
    ):
        """Initialize _MaintenanceThread."""
        threading.Thread.__init__(self, name="MaintenanceThread")
        self.daemon = True
        self.logger = logger
        self._rs_fixtures = rs_fixtures
        self._maintenance_interval_secs = maintenance_interval_secs
        self.__lifecycle = maintenance_lifecycle
        self._fixture = fixture
        self._auth_options = auth_options

        self._last_exec = time.time()
        # Event set when the thread has been stopped using the 'stop()' method.
        self._is_stopped_evt = threading.Event()
        # Event set when the thread is not performing maintenances.
        self._is_idle_evt = threading.Event()
        self._is_idle_evt.set()
        self._paused = False

    def run(self):
        """Execute the thread."""
        if not self._rs_fixtures:
            self.logger.warning("No replica set on which to run maintenances.")
            return

        try:  
            while True:  
                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    break

                if self._paused:
                    self.__lifecycle.wait_for_action_interval(self._maintenance_interval_secs)
                    continue

                found_idle_request = self.__lifecycle.poll_for_idle_request()
                if found_idle_request:
                    self.__lifecycle.send_idle_acknowledgement()
                    continue

                rs_fixture = self._rs_fixtures[0]
                secondaries = rs_fixture.get_secondaries()
                chosen = random.choice(secondaries)
                self.logger.info(
                    "Chose secondary on port %d of replica set '%s' for step up attempt.",
                    chosen.port,
                    rs_fixture.replset_name,
                )
                client = fixture_interface.build_client(chosen, self._auth_options)

                self.logger.info("Putting secondary into maintenance mode...")  
                self._toggle_maintenance_mode(client, enable=True)  
  
                self.logger.info(f"Sleeping for {self._maintenance_interval_secs} seconds...")  
                self.__lifecycle.wait_for_action_interval(self._maintenance_interval_secs)  
  
                self.logger.info("Disabling maintenance mode...")  
                self._toggle_maintenance_mode(client, enable=False)  
  
                self.logger.info(f"Sleeping for {self._maintenance_interval_secs} seconds...")  
                self.__lifecycle.wait_for_action_interval(self._maintenance_interval_secs)
        except Exception as e:
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception(f"Maintenance Thread threw exception: {e}")
            # The event should be signaled whenever the thread is not performing maintenances.
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
        self._paused = True
        self.__lifecycle.mark_test_finished()

        # Wait until we are no longer executing maintenances.
        self._is_idle_evt.wait()
        # Check if the thread is alive in case it has thrown an exception while running.
        self._check_thread()
        self._none_maintenance_mode()


        # Check that fixtures are still running
        for rs_fixture in self._rs_fixtures:
            if not rs_fixture.is_running():
                raise errors.ServerFailure(
                    "ReplicaSetFixture with pids {} expected to be running in"
                    " ContinuousMaintenance, but wasn't.".format(rs_fixture.pids())
                )

    def resume(self):
        """Resume the thread."""
        self._paused = False
        self.__lifecycle.mark_test_started()

    def _check_thread(self):
        if not self.is_alive():
            msg = "The maintenance thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)

    def _none_maintenance_mode(self):
        for rs_fixture in self._rs_fixtures:
            secondaries = rs_fixture.get_secondaries()
            for secondary in secondaries:
                client = fixture_interface.build_client(secondary, self._auth_options)
                self._toggle_maintenance_mode(client, enable=False)  

    def _toggle_maintenance_mode(self, client, enable):  
        """  
        Toggles a secondary node into and out of maintenance mode.  
      
        Args:  
            client (MongoClient): A PyMongo client connected to the secondary node.  
            enable (bool): True to enable maintenance mode, False to disable it.  
        """  
        try:  
            result = client.admin.command('replSetMaintenance', enable)  
            self.logger.info(f"Maintenance mode {'enabled' if enable else 'disabled'}: {result}")  
        except pymongo.errors.OperationFailure as e:
            # Note it is expected to see this log if we are trying to set maintenance mode disabled when we are not in maintenance mode.
            self.logger.info(f"Failed to toggle maintenance mode: {e}")
