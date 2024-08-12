"""Fixture with multiple replica sets for executing JSTests against."""

import copy
import os.path

from buildscripts.resmokelib.testing.fixtures import interface
from buildscripts.resmokelib.utils import dictionary


class MultiReplicaSetFixture(interface.MultiClusterFixture):
    """Fixture which provides JSTests with a set of replica sets to run against."""

    AWAIT_REPL_TIMEOUT_MINS = 5
    AWAIT_REPL_TIMEOUT_FOREVER_MINS = 24 * 60
    CONNECTION_STRING_DB_NAME = "config"
    CONNECTION_STRING_COLL_NAME = "multiReplicaSetFixture"

    def __init__(
        self,
        logger,
        job_num,
        fixturelib,
        dbpath_prefix=None,
        num_replica_sets=2,
        num_nodes_per_replica_set=2,
        common_mongod_options=None,
        per_mongod_options=None,
        per_replica_set_options=None,
        persist_connection_strings=False,
        **common_replica_set_options,
    ):
        """Initialize MultiReplicaSetFixture with different options for the replica set processes."""

        interface.MultiClusterFixture.__init__(self, logger, job_num, fixturelib, dbpath_prefix)

        self.num_replica_sets = num_replica_sets if num_replica_sets else self.config.NUM_REPLSETS
        if self.num_replica_sets < 2:
            raise ValueError("num_replica_sets must be greater or equal to 2")
        self.num_nodes_per_replica_set = num_nodes_per_replica_set

        self.common_mongod_options = self.fixturelib.default_if_none(common_mongod_options, {})
        self.per_mongod_options = self.fixturelib.default_if_none(per_mongod_options, [])
        self.common_replica_set_options = common_replica_set_options
        self.per_replica_set_options = self.fixturelib.default_if_none(per_replica_set_options, [])
        self.persist_connection_strings = persist_connection_strings

        self.auth_options = self.common_replica_set_options.get("auth_options", None)
        # Store this since it is needed by the ContinuousStepdown hook.
        self.all_nodes_electable = self.common_replica_set_options.get("all_nodes_electable", False)

        self.replica_sets = []
        if not self.replica_sets:
            for i in range(self.num_replica_sets):
                rs_name = f"rs{i}"
                mongod_options = copy.deepcopy(self.common_mongod_options)
                if self.per_mongod_options:
                    dictionary.merge_dicts(mongod_options, self.per_mongod_options[i])
                mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, rs_name)
                replica_set_options = self.common_replica_set_options.copy()
                if self.per_replica_set_options:
                    replica_set_options.update(self.per_replica_set_options[i])

                self.replica_sets.append(
                    self.fixturelib.make_fixture(
                        "ReplicaSetFixture",
                        self.logger,
                        self.job_num,
                        replset_name=rs_name,
                        replicaset_logging_prefix=rs_name,
                        num_nodes=self.num_nodes_per_replica_set,
                        mongod_options=mongod_options,
                        **replica_set_options,
                    )
                )

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = []
        for replica_set in self.replica_sets:
            out.extend(replica_set.pids())
        if not out:
            self.logger.debug("No replica sets when gathering multi replicaset fixture pids.")
        return out

    def setup(self):
        """Set up the replica sets."""
        for replica_set in self.replica_sets:
            replica_set.setup()

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        # Wait for each of the replica sets
        for replica_set in self.replica_sets:
            replica_set.await_ready()
        if self.persist_connection_strings:
            docs = [
                {"_id": i, "connectionString": replica_set.get_driver_connection_url()}
                for (i, replica_set) in enumerate(self.replica_sets)
            ]
            primary_client = interface.build_client(
                self.replica_sets[0].get_primary(), self.auth_options
            )
            primary_coll = primary_client[self.CONNECTION_STRING_DB_NAME][
                self.CONNECTION_STRING_COLL_NAME
            ]
            primary_coll.insert_many(docs)

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
            raise self.fixturelib.ServerFailure(teardown_handler.get_error_message())

    def is_running(self):
        """Return true if all replica sets are still operating."""
        return all(replica_set.is_running() for replica_set in self.replica_sets)

    def get_num_replsets(self):
        """Return the number of replica sets."""
        return self.num_replica_sets

    def get_replset(self, index):
        """Return the ReplicaSetFixture for the replica set at the given index."""
        if not self.replica_sets:
            raise ValueError("Must call setup() before calling get_replset")
        return self.replica_sets[index]

    def get_replsets(self):
        """Return the ReplicaSetFixtures for all the replica sets."""
        if not self.replica_sets:
            raise ValueError("Must call setup() before calling get_replsets")
        return self.replica_sets

    def get_internal_connection_string(self):
        """Return the internal connection string to the replica set that tests should connect to."""
        if not self.replica_sets:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")
        return self.replica_sets[0].get_internal_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL to the replica set that tests should connect to."""
        if not self.replica_sets:
            raise ValueError("Must call setup() before calling get_driver_connection_url")
        return self.replica_sets[0].get_driver_connection_url()

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        for replica_set in self.replica_sets:
            output += replica_set.get_node_info()
        return output

    def get_independent_clusters(self):
        """Return the independent replica sets."""
        return self.replica_sets.copy()
