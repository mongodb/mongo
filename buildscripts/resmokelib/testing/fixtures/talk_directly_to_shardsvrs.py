"""Fixture with shardsvrs without mongos/mongoq for executing JSTests against."""

import os.path

import pymongo
import pymongo.write_concern

import buildscripts.resmokelib.testing.fixtures.interface as interface
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib


class TalkDirectlyToShardsvrsFixture(interface.MultiClusterFixture):
    """Fixture which provides JSTests with a set of shardsvrs and a config svr set to run against."""

    def __init__(self, logger, job_num, fixturelib, common_mongod_options=None, dbpath_prefix=None,
                 preserve_dbpath=False, num_replica_sets=1, num_nodes_per_replica_set=3,
                 start_initial_sync_node=False, write_concern_majority_journal_default=None,
                 auth_options=None, replset_config_options=None, voting_secondaries=True,
                 all_nodes_electable=False, use_replica_set_connection_string=None,
                 linear_chain=False, mixed_bin_versions=None, default_read_concern=None,
                 default_write_concern=None):
        """Initialize TalkDirectlyToShardsvrsFixture with different options for the replica set processes."""

        interface.MultiClusterFixture.__init__(self, logger, job_num, fixturelib,
                                               dbpath_prefix=dbpath_prefix)

        self.common_mongod_options = self.fixturelib.default_if_none(common_mongod_options, {})
        self.preserve_dbpath = preserve_dbpath
        self.start_initial_sync_node = start_initial_sync_node
        self.write_concern_majority_journal_default = write_concern_majority_journal_default
        self.auth_options = auth_options
        self.replset_config_options = self.fixturelib.default_if_none(replset_config_options, {})
        self.voting_secondaries = voting_secondaries
        self.all_nodes_electable = all_nodes_electable
        self.use_replica_set_connection_string = use_replica_set_connection_string
        self.default_read_concern = default_read_concern
        self.default_write_concern = default_write_concern
        self.mixed_bin_versions = self.fixturelib.default_if_none(mixed_bin_versions,
                                                                  self.config.MIXED_BIN_VERSIONS)
        self.mixed_bin_versions_config = self.mixed_bin_versions

        # Use the values given from the command line if they exist for linear_chain and num_nodes.
        linear_chain_option = self.fixturelib.default_if_none(self.config.LINEAR_CHAIN,
                                                              linear_chain)
        self.linear_chain = linear_chain_option if linear_chain_option else linear_chain
        self.num_nodes_per_replica_set = num_nodes_per_replica_set if num_nodes_per_replica_set \
            else self.config.NUM_REPLSET_NODES
        self.num_replica_sets = num_replica_sets if num_replica_sets else self.config.NUM_REPLSETS

        if self.num_replica_sets != 1:
            raise ValueError("num_replica_sets must be equal to 1")

        self.configsvr = None
        # Store the replica sets in an array because in the future we may want to use this
        # fixture with more than one replica set.
        self.replica_sets = []

        if not self.configsvr:
            rs_name = "config"

            replset_config_options = self.replset_config_options.copy()
            replset_config_options["configsvr"] = True

            mongod_options = self.common_mongod_options.copy()
            mongod_options["configsvr"] = ""
            mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, rs_name)
            mongod_options["replSet"] = rs_name
            mongod_options["storageEngine"] = "wiredTiger"

            self.configsvr = self.fixturelib.make_fixture(
                "ReplicaSetFixture", self.logger, self.job_num, mongod_options=mongod_options,
                preserve_dbpath=self.preserve_dbpath, num_nodes=self.num_nodes_per_replica_set,
                auth_options=self.auth_options, replset_config_options=replset_config_options,
                mixed_bin_versions=self.mixed_bin_versions, replicaset_logging_prefix=rs_name,
                use_replica_set_connection_string=self.use_replica_set_connection_string,
                all_nodes_electable=self.all_nodes_electable)

        if not self.replica_sets:
            for i in range(self.num_replica_sets):
                rs_name = f"rs{i}"
                mongod_options = self.common_mongod_options.copy()
                mongod_options["shardsvr"] = ""
                mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, rs_name)
                mongod_options["serverless"] = True

                self.replica_sets.append(
                    self.fixturelib.make_fixture(
                        "ReplicaSetFixture", self.logger, self.job_num,
                        mongod_options=mongod_options, preserve_dbpath=self.preserve_dbpath,
                        num_nodes=self.num_nodes_per_replica_set, auth_options=self.auth_options,
                        replset_config_options=self.replset_config_options,
                        mixed_bin_versions=self.mixed_bin_versions,
                        replicaset_logging_prefix=rs_name,
                        use_replica_set_connection_string=self.use_replica_set_connection_string,
                        all_nodes_electable=self.all_nodes_electable, replset_name=rs_name))

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = []
        for replica_set in self.replica_sets:
            out.extend(replica_set.pids())
        if not out:
            self.logger.debug('No replica sets when gathering multi replicaset fixture pids.')
        return out

    def setup(self):
        """Set up the fixture."""
        #Set up the config server replica set.
        self.configsvr.setup()
        #Set up the replica sets.
        for replica_set in self.replica_sets:
            replica_set.setup()

    def refresh_logical_session_cache(self, target):
        """Refresh logical session cache with no timeout."""
        primary = target.get_primary().mongo_client()
        try:
            primary.admin.command({"refreshLogicalSessionCacheNow": 1})
        except pymongo.errors.OperationFailure as err:
            if err.code != self._WRITE_CONCERN_FAILED:
                raise err
            self.logger.info("Ignoring write concern timeout for refreshLogicalSessionCacheNow "
                             "command and continuing to wait")
            target.await_last_op_committed(target.AWAIT_REPL_TIMEOUT_FOREVER_MINS * 60)

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        # Wait for the config server
        self.configsvr.await_ready()

        # Wait for each of the shards
        for replica_set in self.replica_sets:
            replica_set.await_ready()

        # Add all the shards
        for replica_set in self.replica_sets:
            connection_string = replica_set.get_internal_connection_string()
            self.logger.info("Adding %s as a shard...", connection_string)

            config_primary = self.configsvr.get_primary()
            config_primary_client = interface.authenticate(config_primary.mongo_client(),
                                                           self.auth_options)

            try:
                config_primary_client.admin.command(
                    {"_configsvrAddShard": connection_string},
                    write_concern=pymongo.write_concern.WriteConcern(w="majority"))
            except:
                self.logger.exception("Error calling addShard for replica set '%s'",
                                      connection_string)
                raise

        # Ensure that the sessions collection gets auto-sharded by the config server
        self.logger.info("Sending refresh logical session cache to configsvr")
        self.refresh_logical_session_cache(self.configsvr)

        for replica_set in self.replica_sets:
            self.logger.info("Sending refresh logical session cache to shards")
            self.refresh_logical_session_cache(replica_set)

    def _do_teardown(self, mode=None):
        """Shut down the replica sets."""
        self.logger.info("Stopping all replica sets...")

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.warning("All replica sets were expected to be running, but weren't.")

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        for replica_set in self.replica_sets:
            teardown_handler.teardown(replica_set, "replica_set", mode=mode)

        teardown_handler.teardown(self.configsvr, "config server", mode=mode)

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped all replica sets.")
        else:
            self.logger.error("Stopping the fixture failed.")
            raise self.fixturelib.ServerFailure(teardown_handler.get_error_message())

    def is_running(self):
        """Return true if all replica sets are still operating."""
        return all(replica_set.is_running() for replica_set in self.replica_sets)

    def get_num_replsets(self):
        """Return the number of replica sets."""
        return self.num_replica_sets

    def get_replset(self, index):
        """Return the ReplicaSetFixture for the replica set at the given index."""
        return self.replica_sets[index]

    def get_replsets(self):
        """Return the ReplicaSetFixtures for all the replica sets."""
        return self.replica_sets

    def get_internal_connection_string(self):
        """Return the internal connection string to the replica set that currently starts out owning the data."""
        return self.replica_sets[0].get_internal_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL to the replica set that currently starts out owning the data."""
        return self.replica_sets[0].get_driver_connection_url()

    def get_independent_clusters(self):
        """Return the replica sets involved in the cluster."""
        return self.replica_sets.copy()

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        for replica_set in self.replica_sets:
            output += replica_set.get_node_info()
        return output
