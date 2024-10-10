"""Fixture with multiple sharded clusters for executing JSTests against."""

import copy
import os.path

import pymongo

from buildscripts.resmokelib.testing.fixtures import interface
from buildscripts.resmokelib.utils import dictionary


class MultiShardedClusterFixture(interface.MultiClusterFixture):
    """Fixture which provides JSTests with a set of sharded clusters to run against."""

    AWAIT_REPL_TIMEOUT_MINS = 5
    AWAIT_REPL_TIMEOUT_FOREVER_MINS = 24 * 60
    CONNECTION_STRING_DB_NAME = "config"
    CONNECTION_STRING_COLL_NAME = "multiShardedClusterFixture"

    def __init__(
        self,
        logger,
        job_num,
        fixturelib,
        dbpath_prefix=None,
        num_sharded_clusters=2,
        common_mongod_options=None,
        per_mongod_options=None,
        per_sharded_cluster_options=None,
        persist_connection_strings=False,
        **common_sharded_cluster_options,
    ):
        """Initialize MultiShardedClusterFixture with different options for the sharded cluster processes."""

        interface.MultiClusterFixture.__init__(self, logger, job_num, fixturelib, dbpath_prefix)

        if num_sharded_clusters < 2:
            raise ValueError("num_sharded_clusters must be greater or equal to 2")
        self.num_sharded_clusters = num_sharded_clusters

        self.common_mongod_options = self.fixturelib.default_if_none(common_mongod_options, {})
        self.per_mongod_options = self.fixturelib.default_if_none(per_mongod_options, [])
        self.common_sharded_cluster_options = common_sharded_cluster_options
        self.per_sharded_cluster_options = self.fixturelib.default_if_none(
            per_sharded_cluster_options, []
        )
        self.persist_connection_strings = persist_connection_strings

        self.sharded_clusters = []
        if not self.sharded_clusters:
            for i in range(self.num_sharded_clusters):
                cluster_name = f"cl{i}"
                dbpath_prefix = os.path.join(self._dbpath_prefix, cluster_name)
                mongod_options = copy.deepcopy(self.common_mongod_options)
                if self.per_mongod_options:
                    dictionary.merge_dicts(mongod_options, self.per_mongod_options[i])
                sharded_cluster_options = self.common_sharded_cluster_options.copy()
                if self.per_sharded_cluster_options:
                    sharded_cluster_options.update(self.per_sharded_cluster_options[i])

                self.sharded_clusters.append(
                    self.fixturelib.make_fixture(
                        "ShardedClusterFixture",
                        self.logger,
                        self.job_num,
                        dbpath_prefix=dbpath_prefix,
                        cluster_logging_prefix=cluster_name,
                        mongod_options=mongod_options,
                        **sharded_cluster_options,
                    )
                )

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = []
        for sharded_cluster in self.sharded_clusters:
            out.extend(sharded_cluster.pids())
        if not out:
            self.logger.debug(
                "No sharded clusters when gathering multi sharded cluster fixture pids."
            )
        return out

    def setup(self):
        """Set up the sharded clusters."""
        for sharded_cluster in self.sharded_clusters:
            sharded_cluster.setup()

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        # Wait for each of the sharded clusters
        for sharded_cluster in self.sharded_clusters:
            sharded_cluster.await_ready()
        if self.persist_connection_strings:
            docs = [
                {"_id": i, "connectionString": sharded_cluster.get_driver_connection_url()}
                for (i, sharded_cluster) in enumerate(self.sharded_clusters)
            ]
            client = pymongo.MongoClient(self.sharded_clusters[0].get_driver_connection_url())
            coll = client[self.CONNECTION_STRING_DB_NAME][self.CONNECTION_STRING_COLL_NAME]
            coll.insert_many(docs)

    def feature_flag_present_and_enabled(self, feature_flag_name):
        """Return true if the given feature flag is present and enabled on all sharded clusters."""
        for sharded_cluster in self.sharded_clusters:
            if not sharded_cluster.feature_flag_present_and_enabled(feature_flag_name):
                return False
        return True

    def _do_teardown(self, mode=None):
        """Shut down the sharded clusters."""
        self.logger.info("Stopping all sharded clusters...")

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.warning("All sharded clusters were expected to be running, but weren't.")

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        for sharded_cluster in self.sharded_clusters:
            teardown_handler.teardown(sharded_cluster, "sharded_cluster", mode=mode)

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped all sharded clusters.")
        else:
            self.logger.error("Stopping the fixture failed.")
            raise self.fixturelib.ServerFailure(teardown_handler.get_error_message())

    def is_running(self):
        """Return true if all sharded clusters are still operating."""
        return all(sharded_cluster.is_running() for sharded_cluster in self.sharded_clusters)

    def get_num_sharded_clusters(self):
        """Return the number of sharded clusters."""
        return self.num_sharded_clusters

    def get_sharded_cluster(self, index):
        """Return the ShardedClusterFixture for the sharded cluster at the given index."""
        if not self.sharded_clusters:
            raise ValueError("Must call setup() before calling get_sharded_cluster")
        return self.sharded_clusters[index]

    def get_sharded_clusters(self):
        """Return the ShardedClusterFixtures for all the sharded clusters."""
        if not self.sharded_clusters:
            raise ValueError("Must call setup() before calling get_sharded_clusters")
        return self.sharded_clusters

    def get_internal_connection_string(self):
        """Return the internal connection string to the sharded cluster that tests should connect to."""
        if not self.sharded_clusters:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")
        return self.sharded_clusters[0].get_internal_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL to the sharded cluster that tests should connect to."""
        if not self.sharded_clusters:
            raise ValueError("Must call setup() before calling get_driver_connection_url")
        return self.sharded_clusters[0].get_driver_connection_url()

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        for sharded_cluster in self.sharded_clusters:
            output += sharded_cluster.get_node_info()
        return output

    def get_independent_clusters(self):
        """Return the independent sharded clusters."""
        return self.sharded_clusters.copy()
