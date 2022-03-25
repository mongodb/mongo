"""Standalone replicator fixture to handle launching and stopping a replicator binary."""

import json
import time
from urllib import request

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

    def setup(self):
        """Launch the replicator webserver to begin accepting replicator commands."""
        self._launch_replicator_process()

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = [x.pid for x in [self.replicator] if x is not None]
        if not out:
            self.logger.debug('Replicator not running when gathering replicator fixture pid.')
        return out

    def await_ready(self):
        """
        Block until the fixture can be used for testing.

        NOOP since the binary is fully launched in `setup`.
        """
        pass

    def _launch_replicator_process(self):
        """Launch the replicator binary."""

        replicator = self.fixturelib.generic_program(self.logger, [self.executable], self.job_num,
                                                     test_id=None, process_kwargs=None,
                                                     **self.cli_options)
        try:
            self.logger.info("Launch replicator webserver...\n%s", replicator.as_command())
            replicator.start()
            self.logger.info("Replicator launched with pid %d on port %d.", replicator.pid,
                             self.port)
        except Exception as err:
            msg = "Failed to launch replicator: {}".format(err)
            self.logger.exception(msg)
            raise self.fixturelib.ServerFailure(msg)

        self.replicator = replicator

    def start(self):
        """Start the replication process by sending the replicator a command."""
        url = self.get_api_url() + '/api/v1/start'
        # Right now we set reversible to false, at some point this could be an
        # argument to start.
        data = '{"reversible": false, "source": "cluster0", "destination": "cluster1"}'.encode(
            'ascii')
        headers = {'Content-Type': 'application/json'}

        req = request.Request(url=url, data=data, headers=headers)
        self.logger.info("Sending start command to replicator: %s", req.data)
        response = request.urlopen(req).read().decode('ascii')
        self.logger.info("Replicator start command response was: %s", response)

        if not json.loads(response)["success"]:
            msg = f"Replicator failed to start: {response}"
            self.logger.exception(msg)
            raise self.fixturelib.ServerFailure(msg)

    def commit(self):
        """Commit the migration. This currently will just sleep for a quiesce period."""
        self.logger.info("Sleeping for %d s to allow replicator to finish up.", self.quiesce_period)
        time.sleep(self.quiesce_period)
        self.logger.info("Done sleeping through quiesce period.")

    def stop(self, mode=None):
        """Stop the replicator binary."""
        mode = interface.TeardownMode.TERMINATE if mode is None else mode

        self.logger.info("Stopping replicator with pid %d...", self.replicator.pid)
        if not self._is_process_running():
            exit_code = self.replicator.poll()
            msg = ("Replicator was expected to be running, but wasn't. "
                   "Process exited with code {:d}.").format(exit_code)
            self.logger.warning(msg)
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
        return self._is_process_running()

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        raise NotImplementedError("Replicator cannot have an internal connection string.")

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        raise NotImplementedError("Replicator cannot have a driver connection URL.")

    def get_api_url(self):
        """Return the URL used to send the replicator commands."""
        return f'http://localhost:{self.port}'

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
