"""Fixture with multiple replica sets for executing JSTests against."""

import os.path

from buildscripts.resmokelib import config
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import logging
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.fixtures import interface
from buildscripts.resmokelib.testing.fixtures import replicaset


class TenantMigrationFixture(interface.Fixture):  # pylint: disable=too-many-instance-attributes
    """Fixture which provides JSTests with a set of replica sets to run tenant migration against."""

    def __init__(  # pylint: disable=too-many-arguments,too-many-locals
            self, logger, job_num, common_mongod_options=None, per_mongod_options=None,
            dbpath_prefix=None, preserve_dbpath=False, num_replica_sets=1,
            num_nodes_per_replica_set=2, start_initial_sync_node=False,
            write_concern_majority_journal_default=None, auth_options=None,
            replset_config_options=None, voting_secondaries=True, all_nodes_electable=False,
            use_replica_set_connection_string=None, linear_chain=False, mixed_bin_versions=None,
            default_read_concern=None, default_write_concern=None):
        """Initialize TenantMigrationFixture with different options for the replica set processes."""

        interface.Fixture.__init__(self, logger, job_num, dbpath_prefix=dbpath_prefix)

        self.common_mongod_options = utils.default_if_none(common_mongod_options, {})
        self.per_mongod_options = utils.default_if_none(per_mongod_options, {})
        self.preserve_dbpath = preserve_dbpath
        self.start_initial_sync_node = start_initial_sync_node
        self.write_concern_majority_journal_default = write_concern_majority_journal_default
        self.auth_options = auth_options
        self.replset_config_options = utils.default_if_none(replset_config_options, {})
        self.voting_secondaries = voting_secondaries
        self.all_nodes_electable = all_nodes_electable
        self.use_replica_set_connection_string = use_replica_set_connection_string
        self.default_read_concern = default_read_concern
        self.default_write_concern = default_write_concern
        self.mixed_bin_versions = utils.default_if_none(mixed_bin_versions,
                                                        config.MIXED_BIN_VERSIONS)
        self.mixed_bin_versions_config = self.mixed_bin_versions

        # Use the values given from the command line if they exist for linear_chain and num_nodes.
        linear_chain_option = utils.default_if_none(config.LINEAR_CHAIN, linear_chain)
        self.linear_chain = linear_chain_option if linear_chain_option else linear_chain
        self.num_nodes_per_replica_set = num_nodes_per_replica_set if num_nodes_per_replica_set \
            else config.NUM_REPLSET_NODES
        self.num_replica_sets = num_replica_sets if num_replica_sets else config.NUM_REPLSETS
        if self.num_replica_sets < 2:
            raise ValueError("num_replica_sets must be greater or equal to 2")

        self.replica_sets = []

        # The ReplicaSetFixture for the replica set that starts out owning the data (i.e. the
        # replica set that driver should connect to when running commands).
        self.replica_set_with_tenant = None

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = []
        for replica_set in self.replica_sets:
            out.extend(replica_set.pids())
        if not out:
            self.logger.debug('No replica sets when gathering multi replicaset fixture pids.')
        return out

    def setup(self):
        """Set up the replica sets."""
        if not self.replica_sets:
            for i in range(self.num_replica_sets):
                rs_name = f"rs{i}"
                mongod_options = self.common_mongod_options.copy()
                mongod_options.update(self.per_mongod_options[i])
                mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, rs_name)
                mongod_options["replSet"] = rs_name

                self.replica_sets.append(
                    replicaset.ReplicaSetFixture(
                        self.logger, self.job_num, mongod_options=mongod_options,
                        preserve_dbpath=self.preserve_dbpath,
                        num_nodes=self.num_nodes_per_replica_set, auth_options=self.auth_options,
                        replset_config_options=self.replset_config_options,
                        mixed_bin_versions=self.mixed_bin_versions,
                        replicaset_logging_prefix=rs_name,
                        use_replica_set_connection_string=self.use_replica_set_connection_string))

            self.replica_set_with_tenant = self.replica_sets[0]

        # Start up each of the replica sets
        for replica_set in self.replica_sets:
            replica_set.setup()

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        # Wait for each of the replica sets
        for replica_set in self.replica_sets:
            replica_set.await_ready()

    def _do_teardown(self, mode=None):
        """Shut down the replica sets."""
        self.logger.info("Stopping all replica sets...")

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.warning("All replica sets were expected to be running, but weren't.")

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        for replica_set in self.replica_sets:
            teardown_handler.teardown(replica_set, "replica_set", mode=mode)

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped all replica sets.")
        else:
            self.logger.error("Stopping the fixture failed.")
            raise errors.ServerFailure(teardown_handler.get_error_message())

    def is_running(self):
        """Return true if all replica sets are still operating."""
        return all(replica_set.is_running() for replica_set in self.replica_sets)

    def get_num_replica_sets(self):
        """Return the number of replica sets."""
        return self.num_replica_sets

    def get_replset(self, index):
        """Return the ReplicaSetFixture for the replica set at the given index."""
        if not self.replica_sets:
            raise ValueError("Must call setup() before calling get_replset")
        return self.replica_sets[index]

    def get_internal_connection_string(self):
        """Return the internal connection string to the replica set that currently starts out owning the data."""
        if not self.replica_sets:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")
        return self.replica_set_with_tenant.get_internal_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL to the replica set that currently starts out owning the data."""
        if not self.replica_set_with_tenant:
            raise ValueError("Must call setup() before calling get_driver_connection_url")
        return self.replica_set_with_tenant.get_driver_connection_url()

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        for replica_set in self.replica_sets:
            output += replica_set.get_node_info()
