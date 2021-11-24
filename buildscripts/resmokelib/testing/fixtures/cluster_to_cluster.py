"""Fixture with two clusters (for cluster to cluster replications) for executing JSTests against."""

import copy
import os.path

import buildscripts.resmokelib.testing.fixtures.interface as interface


class ClusterToClusterFixture(interface.MultiClusterFixture):  # pylint: disable=too-many-instance-attributes
    """Fixture which provides two clusters to perform a cluster to cluster replication."""

    def __init__(  # pylint: disable=too-many-arguments,too-many-locals
            self, logger, job_num, fixturelib, cluster0_options, cluster1_options,
            dbpath_prefix=None, preserve_dbpath=False):
        """Initialize with different options for the clusters."""

        interface.MultiClusterFixture.__init__(self, logger, job_num, fixturelib,
                                               dbpath_prefix=dbpath_prefix)

        self.clusters = []
        self.both_cluster_options = []
        parsed_options = [
            self.fixturelib.default_if_none(copy.deepcopy(cluster0_options), {}),
            self.fixturelib.default_if_none(copy.deepcopy(cluster1_options), {})
        ]

        self.preserve_dbpath = preserve_dbpath

        # The usual command line params can lead to messy behavior in unlike topologies, since they
        # may override the cluster-to-cluster behavior specified elsewhere on both clusters in an
        # unexpected way. Therefore, forbid them.
        if any(
                v is not None for v in (self.config.MIXED_BIN_VERSIONS, self.config.NUM_SHARDS,
                                        self.config.NUM_REPLSET_NODES)):
            raise ValueError(
                "ClusterToClusterFixture options must be specified through 'cluster0_options' and 'cluster1_options'."
            )

        for i, cluster_options in enumerate(parsed_options):
            cluster_options["settings"] = self.fixturelib.default_if_none(
                cluster_options["settings"], {})
            if "preserve_dbpath" not in cluster_options["settings"]\
                or cluster_options["settings"]["preserve_dbpath"] is None:
                cluster_options["settings"]["preserve_dbpath"] = self.preserve_dbpath

            cluster_options["settings"]["dbpath_prefix"] = os.path.join(
                self._dbpath_prefix, f"cluster{i}")

            if cluster_options["class"] == "ReplicaSetFixture":
                cluster_options["settings"]["replicaset_logging_prefix"] = f"cl{i}"
            elif cluster_options["class"] == "ShardedClusterFixture":
                cluster_options["settings"]["cluster_logging_prefix"] = f"cl{i}"
            else:
                raise ValueError(f"Illegal fixture class: {cluster_options['class']}")

            self.logger.info(f"Cluster{i} configured with settings: {cluster_options}")

        self.both_cluster_options = parsed_options

        # The cluster that starts off with the data.
        self.source_cluster_index = 0

        for cluster_options in self.both_cluster_options:
            cluster = self.fixturelib.make_fixture(cluster_options["class"], self.logger,
                                                   self.job_num, **cluster_options["settings"])
            self.clusters.append(cluster)

    def setup(self):
        """Set up the cluster to cluster fixture according to the options provided."""

        for i, cluster in enumerate(self.clusters):
            self.logger.info(f"Setting up cluster {i}.")
            cluster.setup()

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = []
        for i, cluster in enumerate(self.clusters):
            self.logger.info(f"Gathering cluster {i} pids: {cluster.pids()}")
            out.extend(cluster.pids())
        if not out:
            self.logger.debug('No clusters when gathering cluster to cluster fixture pids.')
        return out

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        # Wait for each of the clusters.
        for cluster in self.clusters:
            cluster.await_ready()

    def _do_teardown(self, mode=None):
        """Shut down the clusters."""
        self.logger.info("Stopping all clusters...")

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.warning("All clusters were expected to be running, but weren't.")

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        for i, cluster in enumerate(self.clusters):
            teardown_handler.teardown(cluster, f"cluster {i}", mode=mode)

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped all clusters.")
        else:
            self.logger.error("Stopping the fixture failed.")
            raise self.fixturelib.ServerFailure(teardown_handler.get_error_message())

    def is_running(self):
        """Return true if all clusters are still operating."""
        return all(cluster.is_running() for cluster in self.clusters)

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        for cluster in self.clusters:
            output += cluster.get_node_info()
        return output

    def get_driver_connection_url(self):
        """Return the driver connection URL to the cluster that starts out owning the data."""
        if not self.clusters:
            raise ValueError("Must call setup() before calling get_driver_connection_url")
        return self.clusters[self.source_cluster_index].get_driver_connection_url()

    def get_internal_connection_string(self):
        """Return the internal connection string to the cluster that starts out owning the data."""
        if not self.clusters:
            raise ValueError("Must call setup() before calling get_internal_connection_string")
        return self.clusters[0].get_internal_connection_string()

    def get_independent_clusters(self):
        """Return the clusters involved in cluster to cluster replication."""
        return self.clusters.copy()

    def reverse_replication_direction(self):
        """Swap the source and destination clusters."""
        self.source_cluster_index = 1 - self.source_cluster_index
