"""Fixture with multiple clusters for executing bulkWrite overridden tests against."""

import os.path

from buildscripts.resmokelib.testing.fixtures import interface


class BulkWriteFixture(interface.MultiClusterFixture):
    """Fixture which provides JSTests with a set of clusters to run tests against."""

    def __init__(
        self,
        logger,
        job_num,
        fixturelib,
        cluster_options,
        dbpath_prefix=None,
        preserve_dbpath=False,
        requires_auth=False,
    ):
        """Initialize BulkWriteFixture with different options."""

        interface.MultiClusterFixture.__init__(
            self, logger, job_num, fixturelib, dbpath_prefix=dbpath_prefix
        )

        self.setup_complete = False
        self.clusters = []

        # cluster_options will be used for the bulkWrite cluster.
        cluster_options["settings"] = self.fixturelib.default_if_none(
            cluster_options["settings"], {}
        )
        if (
            "preserve_dbpath" not in cluster_options["settings"]
            or cluster_options["settings"]["preserve_dbpath"] is None
        ):
            cluster_options["settings"]["preserve_dbpath"] = preserve_dbpath

        # The "dbpath_prefix" needs to be under "settings" for replicasets
        # but also under "mongod_options" for sharded clusters.
        cluster_options["settings"]["dbpath_prefix"] = os.path.join(
            self._dbpath_prefix, "bulkWriteCluster"
        )

        if cluster_options["class"] == "ReplicaSetFixture":
            cluster_options["settings"]["replicaset_logging_prefix"] = "bw"
            cluster_options["settings"]["dbpath_prefix"] = os.path.join(
                self._dbpath_prefix, "bulkWriteCluster"
            )
        elif cluster_options["class"] == "ShardedClusterFixture":
            cluster_options["settings"]["cluster_logging_prefix"] = "bw"
        else:
            raise ValueError(f"Illegal fixture class: {cluster_options['class']}")

        self.clusters.append(
            self.fixturelib.make_fixture(
                cluster_options["class"], self.logger, self.job_num, **cluster_options["settings"]
            )
        )

        # The cluster where normal writes will be executed has set options.
        normal_cluster_options = {}
        normal_cluster_options["settings"] = {}
        normal_cluster_options["settings"]["mongod_options"] = {}
        normal_cluster_options["settings"]["mongod_options"]["set_parameters"] = {}
        normal_cluster_options["settings"]["mongod_options"]["set_parameters"][
            "enableTestCommands"
        ] = 1

        normal_cluster_options["settings"]["num_nodes"] = 1
        normal_cluster_options["settings"]["use_replica_set_connection_string"] = True

        normal_cluster_options["settings"]["replicaset_logging_prefix"] = "nc"
        normal_cluster_options["settings"]["dbpath_prefix"] = os.path.join(
            self._dbpath_prefix, "normalCluster"
        )
        self.clusters.append(
            self.fixturelib.make_fixture(
                "ReplicaSetFixture",
                self.logger,
                self.job_num,
                **normal_cluster_options["settings"],
                replset_name="rs1",
            )
        )

    def pids(self):
        """Return: pids owned by this fixture if any."""
        out = []
        for cluster in self.clusters:
            out.extend(cluster.pids())
        if not out:
            self.logger.debug("No clusters when gathering multi replicaset fixture pids.")
        return out

    def setup(self):
        """Set up the clusters."""
        for cluster in self.clusters:
            cluster.setup()
        self.setup_complete = True

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        # Wait for each of the clusters
        for cluster in self.clusters:
            cluster.await_ready()

    def _do_teardown(self, mode=None):
        """Shut down the clusters."""
        self.logger.info("Stopping all clusters...")

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.warning("All clusters were expected to be running, but weren't.")

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        for cluster in self.clusters:
            teardown_handler.teardown(cluster, "bulkWriteCluster", mode=mode)

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped all clusters.")
        else:
            self.logger.error("Stopping the fixture failed.")
            raise self.fixturelib.ServerFailure(teardown_handler.get_error_message())
        self.setup_complete = False

    def is_running(self):
        """Return true if all clusters are still operating."""
        return all(cluster.is_running() for cluster in self.clusters)

    def get_internal_connection_string(self):
        """Return the internal connection string to the replica set that currently starts out owning the data."""
        if not self.setup_complete:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")
        return self.clusters[0].get_internal_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL to the replica set that currently starts out owning the data."""
        if not self.setup_complete:
            raise ValueError("Must call setup() before calling get_driver_connection_url")
        return self.clusters[0].get_driver_connection_url()

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        for cluster in self.clusters:
            output += cluster.get_node_info()
        return output

    def get_independent_clusters(self):
        """Return all of the clusters in the fixture."""
        return self.clusters.copy()

    def get_testable_clusters(self):
        """Return the clusters we want to be modified by hooks."""
        return [self.clusters[0]]
