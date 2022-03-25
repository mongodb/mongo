"""Multiple replicator fixture to handle launching and stopping multiple replicator binaries."""

import buildscripts.resmokelib.testing.fixtures.interface as interface


class MultipleReplicatorFixture(interface.Fixture):
    """Fixture which spins up multiple replicators."""

    REGISTERED_NAME = "MultipleReplicatorFixture"

    def __init__(  # pylint: disable=too-many-arguments
            self, logger, job_num, fixturelib, executable, quiesce_period=5, cli_options=None,
            num_replicators=2, shard_ids=None):
        """Initialize ReplicatorFixture with different options for the replicator process."""
        interface.Fixture.__init__(self, logger, job_num, fixturelib)

        self.executable = executable
        self.quiesce_period = quiesce_period
        self.cli_options = self.fixturelib.default_if_none(cli_options, {})
        self.shard_ids = self.fixturelib.default_if_none(shard_ids, [])
        self.num_replicators = num_replicators

        # The running replicator processes.
        self.replicators = []

        if num_replicators < 2:
            raise ValueError("The MultipleReplicatorFixture requires at least 2 replicators")

        for i in range(num_replicators):
            individual_replicator_logger = self.fixturelib.new_fixture_node_logger(
                "MultipleReplicatorFixture", self.job_num, f"replicator{i}")
            replicator = self.fixturelib.make_fixture(
                "ReplicatorFixture", individual_replicator_logger, self.job_num,
                executable=executable, quiesce_period=quiesce_period, cli_options=cli_options)
            self.replicators.append(replicator)

    def setup(self):
        """
        Set up the multiple replicators.

        For each replicator set the shard ID and call setup, which launches the replicator process.
        """
        if self.num_replicators != len(self.shard_ids):
            raise ValueError(
                "The number of replicators must match the number of shard ids provided.")

        for i, rep in enumerate(self.replicators):
            rep.set_cli_options({"id": self.shard_ids[i]})
            rep.setup()

    def pids(self):
        """:return: pids owned by this fixture if any."""
        pids = []
        for rep in self.replicators:
            pids.extend(rep.pids())
        if len(pids) == 0:
            self.logger.debug('Replicators not running when gathering replicator fixture pids.')
        return pids

    def await_ready(self):
        """
        Block until the fixture can be used for testing.

        For each replicator, call await_ready().
        """
        for rep in self.replicators:
            rep.await_ready()

    def start(self):
        """Start the replication process by sending the replicator a command."""
        self.logger.info("Starting multiple replicators...\n")
        for i, rep in enumerate(self.replicators):
            self.logger.info("Starting replicator #%d...", i)
            rep.start()

    def commit(self):
        """Commit the migration. This currently will just sleep for a quiesce period."""
        for rep in self.replicators:
            rep.commit()

    def stop(self, mode=None):
        """Stop the replicator binary."""
        self.logger.info("Stopping multiple replicators...\n")
        for i, rep in enumerate(self.replicators):
            try:
                self.logger.info("Stopping replicator #%d...", i)
                rep.stop(mode)
            except Exception as err:
                msg = f"Error stopping replicator #{i}: {err}"
                self.logger.exception(msg)
                raise self.fixturelib.ServerFailure(msg)

    def resume(self):
        """NOOP."""
        pass

    def pause(self):
        """NOOP."""
        pass

    def _do_teardown(self, mode=None):
        """Teardown the fixture."""
        if not self.is_any_process_running():
            self.logger.info("All replicators already stopped; teardown is a NOOP.")
            return

        self.logger.warning("The replicators had not been stopped at the time of teardown.")
        self.stop(mode)

    def is_any_process_running(self):
        """Return true if any of the replicator binaries is running as a process."""
        return any(rep.is_running() for rep in self.replicators)

    def is_running(self):
        """Return true if all of the individual replicator fixtures are running and have not errorred."""
        return all(rep.is_running() for rep in self.replicators)

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        raise NotImplementedError("Multiple replicator cannot have internal connection strings.")

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        raise NotImplementedError("Multiple replicator cannot have driver connection URLs.")

    def get_node_info(self):
        """Return a list of NodeInfo objects."""
        return [rep.get_node_info()[0] for rep in self.replicators]

    def set_cli_options(self, cli_options):
        """Set command line options."""
        for option, value in cli_options.items():
            self.cli_options[option] = value
        for rep in self.replicators:
            rep.set_cli_options(cli_options)

    def set_shard_ids(self, shard_ids):
        """Set the list of shard ids."""
        self.shard_ids = shard_ids
