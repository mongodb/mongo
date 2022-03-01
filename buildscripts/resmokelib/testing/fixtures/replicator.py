"""Standalone replicator fixture to handle launching and stopping a replicator binary."""

import time

import buildscripts.resmokelib.testing.fixtures.interface as interface


class ReplicatorFixture(interface.Fixture):
    """Fixture which spins up a single replicator."""

    REGISTERED_NAME = "ReplicatorFixture"

    def __init__(  # pylint: disable=too-many-arguments
            self, logger, job_num, fixturelib, executable, quiesce_period=5, cli_options=None):
        """Initialize ReplicatorFixture with different options for the replicator process."""
        interface.Fixture.__init__(self, logger, job_num, fixturelib)

        self.executable = executable
        self.quiesce_period = quiesce_period
        self.cli_options = self.fixturelib.default_if_none(cli_options, {})
        self.port = self.fixturelib.get_next_port(job_num)
        self.set_cli_options({"port": self.port})

        # The running replicator process.
        self.replicator = None

        # This denotes whether the fixture itself is running, i.e. if it hasn't errored and hasn't
        # been torn down. The replicator process itself may have been stopped.
        self.fixture_is_running = False

    def setup(self):
        """Since launching the binary starts the replication, we do nothing here."""
        self.fixture_is_running = True

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = [x.pid for x in [self.replicator] if x is not None]
        if not out:
            self.logger.debug('Replicator not running when gathering replicator fixture pid.')
        return out

    def await_ready(self):
        """
        Block until the fixture can be used for testing.

        NOOP by default since on `setup` nothing is done.
        """
        pass

    def start(self):
        """Launch the binary and start the replication process."""
        if "sourceURI" not in self.cli_options or "destinationURI" not in self.cli_options:
            raise ValueError("Cannot launch the replicator without source and destination URIs.")

        replicator = self.fixturelib.generic_program(self.logger, [self.executable], self.job_num,
                                                     test_id=None, process_kwargs=None,
                                                     **self.cli_options)
        try:
            self.logger.info("Starting replicator...\n%s", replicator.as_command())
            replicator.start()
            self.logger.info("Replicator started with pid %d.", replicator.pid)
        except Exception as err:
            msg = "Failed to start replicator: {}".format(err)
            self.logger.exception(msg)
            self.fixture_is_running = False
            raise self.fixturelib.ServerFailure(msg)

        self.replicator = replicator

    def stop(self, mode=None):
        """Stop the replicator binary."""
        self.logger.info("Sleeping for %d s to allow replicator to finish up.", self.quiesce_period)
        time.sleep(self.quiesce_period)
        self.logger.info("Done sleeping through quiesce period.")

        mode = interface.TeardownMode.TERMINATE if mode is None else mode

        self.logger.info("Stopping replicator with pid %d...", self.replicator.pid)
        if not self._is_process_running():
            exit_code = self.replicator.poll()
            msg = ("Replicator was expected to be running, but wasn't. "
                   "Process exited with code {:d}.").format(exit_code)
            self.logger.warning(msg)
            self.fixture_is_running = False
            raise self.fixturelib.ServerFailure(msg)

        self.replicator.stop(mode)
        exit_code = self.replicator.wait()
        # TODO (SERVER-63544): Check to make sure the error code is correct.
        self.logger.info("Process exited with error code {:d}.".format(exit_code))

    def resume(self):
        """NOOP."""
        pass

    def pause(self):
        """NOOP."""
        pass

    def _do_teardown(self, mode=None):
        """Teardown the fixture."""
        self.fixture_is_running = False

        if not self._is_process_running():
            self.logger.info("Replicator already stopped; teardown is a NOOP.")
            return

        self.logger.warning("The replicator had not been stopped at the time of teardown.")
        self.stop(mode)

    def _is_process_running(self):
        """Return true if the replicator binary is running as a process."""
        return self.replicator is not None and self.replicator.poll() is None

    def is_running(self):
        """Return if the fixture is running or has not errorred."""
        return self.fixture_is_running

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        raise NotImplementedError("Replicator cannot have an internal connection string.")

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        raise NotImplementedError("Replicator cannot have a driver connection URL.")

    def get_node_info(self):
        """Return a list of NodeInfo objects."""
        info = interface.NodeInfo(full_name=self.logger.full_name, name=self.logger.name,
                                  port=self.port,
                                  pid=self.replicator.pid if self.replicator is not None else -1)
        return [info]

    def set_cli_options(self, cli_options):
        """Set command line options."""
        for option, value in cli_options.items():
            self.cli_options[option] = value
