"""Sharded cluster fixture for executing JSTests against."""

import os.path
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import pymongo
import pymongo.errors
import yaml

from buildscripts.resmokelib.extensions import (
    delete_extension_configs,
    find_and_generate_extension_configs,
)
from buildscripts.resmokelib.testing.fixtures import _builder, external, interface
from buildscripts.resmokelib.utils.sharded_cluster_util import (
    inject_catalog_metadata_on_the_csrs,
    refresh_logical_session_cache_with_retry,
)


class ShardedClusterFixture(interface.Fixture, interface._DockerComposeInterface):
    """Fixture which provides JSTests with a sharded cluster to run against."""

    AWAIT_SHARDING_INITIALIZATION_TIMEOUT_SECS = 60

    def __init__(
        self,
        logger,
        job_num,
        fixturelib,
        mongos_options=None,
        mongod_executable=None,
        mongod_options=None,
        dbpath_prefix=None,
        preserve_dbpath=False,
        num_shards=1,
        num_rs_nodes_per_shard=1,
        num_mongos=1,
        enable_balancer=True,
        auth_options=None,
        configsvr_options=None,
        shard_options=None,
        cluster_logging_prefix=None,
        config_shard=None,
        use_auto_bootstrap_procedure=None,
        replica_set_endpoint=False,
        random_migrations=False,
        launch_mongot=False,
        load_all_extensions=False,
        set_cluster_parameter=None,
        inject_catalog_metadata=None,
        shard_replset_name_prefix="shard-rs",
        configsvr_replset_name="config-rs",
    ):
        """
        Initialize ShardedClusterFixture with different options for the cluster processes.
        """
        interface.Fixture.__init__(self, logger, job_num, fixturelib, dbpath_prefix=dbpath_prefix)

        if "dbpath" in mongod_options:
            raise ValueError("Cannot specify mongod_options.dbpath")

        self.mongos_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(mongos_options, {})
        )
        # The mongotHost and searchIndexManagementHostAndPort options cannot be set on mongos_options yet because
        # the port value is only assigned in MongoDFixture initialization, which happens later.
        self.launch_mongot = launch_mongot

        # mongod options
        self.mongod_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(mongod_options, {})
        )

        self.load_all_extensions = load_all_extensions or self.config.LOAD_ALL_EXTENSIONS
        if self.load_all_extensions:
            self.loaded_extensions = find_and_generate_extension_configs(
                is_evergreen=self.config.EVERGREEN_TASK_ID,
                logger=self.logger,
                mongod_options=self.mongod_options,
                mongos_options=self.mongos_options,
            )

        self.mongod_executable = mongod_executable
        self.mongod_options["set_parameters"] = self.fixturelib.make_historic(
            mongod_options.get("set_parameters", {})
        ).copy()
        self.mongod_options["set_parameters"]["migrationLockAcquisitionMaxWaitMS"] = (
            self.mongod_options["set_parameters"].get("migrationLockAcquisitionMaxWaitMS", 30000)
        )
        # Extend time for transactions by default to account for slow machines during testing.
        self.mongod_options["set_parameters"]["maxTransactionLockRequestTimeoutMillis"] = (
            self.mongod_options[
                "set_parameters"
            ].get("maxTransactionLockRequestTimeoutMillis", 10 * 1000)
        )

        # Misc other options for the fixture.
        self.config_shard = config_shard
        self.preserve_dbpath = preserve_dbpath
        self.num_shards = num_shards
        self.num_rs_nodes_per_shard = num_rs_nodes_per_shard
        self.num_mongos = num_mongos
        self.auth_options = auth_options
        self.use_auto_bootstrap_procedure = use_auto_bootstrap_procedure
        self.replica_endpoint_mode = replica_set_endpoint
        self.set_cluster_parameter = set_cluster_parameter
        self.inject_catalog_metadata = inject_catalog_metadata
        self.configsvr_replset_name = configsvr_replset_name
        self.shard_replset_name_prefix = shard_replset_name_prefix

        # Options for roles - shardsvr, configsvr.
        self.configsvr_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(configsvr_options, {})
        )
        self.shard_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(shard_options, {})
        )

        # Logging prefix options.
        #  `cluster_logging_prefix` is the logging prefix used in cluster to cluster replication.
        self.cluster_logging_prefix = (
            "" if cluster_logging_prefix is None else f"{cluster_logging_prefix}:"
        )
        self.configsvr_shard_logging_prefix = f"{self.cluster_logging_prefix}configsvr"
        self.rs_shard_logging_prefix = f"{self.cluster_logging_prefix}shard"
        self.mongos_logging_prefix = f"{self.cluster_logging_prefix}mongos"

        if self.num_rs_nodes_per_shard is None:
            raise TypeError("num_rs_nodes_per_shard must be an integer but found None")
        elif isinstance(self.num_rs_nodes_per_shard, int):
            if self.num_rs_nodes_per_shard <= 0:
                raise ValueError("num_rs_nodes_per_shard must be a positive integer")

        # Balancer options
        self.enable_balancer = enable_balancer
        self.random_migrations = random_migrations
        if self.random_migrations:
            if not self.enable_balancer:
                raise ValueError(
                    "random_migrations can only be enabled when balancer is enabled (enable_balancer=True)"
                )

            if (
                "failpoint.balancerShouldReturnRandomMigrations"
                in self.mongod_options["set_parameters"]
            ):
                raise ValueError(
                    "Cannot enable random_migrations because balancerShouldReturnRandomMigrations failpoint is already present in mongod_options"
                )

            # Enable random migrations failpoint
            self.mongod_options["set_parameters"][
                "failpoint.balancerShouldReturnRandomMigrations"
            ] = {"mode": "alwaysOn"}

            # Reduce migration throttling to increase frequency of random migrations
            self.mongod_options["set_parameters"]["balancerMigrationsThrottlingMs"] = (
                self.mongod_options["set_parameters"].get("balancerMigrationsThrottlingMs", 250)
            )  # millis

        self._dbpath_prefix = os.path.join(
            dbpath_prefix if dbpath_prefix else self._dbpath_prefix, self.config.FIXTURE_SUBDIR
        )

        self.configsvr = None
        self.mongos = []
        self.shards = []
        # These mongot-related options will be set after each shard has been setup().
        # They're used to connect a sharded cluster's mongos to last launched mongot.
        self.mongotHost = None
        self.searchIndexManagementHostAndPort = None

        self.is_ready = False

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = []
        if self.configsvr is not None:
            out.extend(self.configsvr.pids())
        else:
            self.logger.debug(
                "Config server not running when gathering sharded cluster fixture pids."
            )
        if self.shards is not None:
            for shard in self.shards:
                out.extend(shard.pids())
        else:
            self.logger.debug("No shards when gathering sharded cluster fixture pids.")
        return out

    def setup(self):
        """Set up the sharded cluster."""

        with ThreadPoolExecutor() as executor:
            tasks = []

            if self.config_shard is None:
                tasks.append(executor.submit(self.configsvr.setup))

            # Start up each of the shards
            for shard in self.shards:
                tasks.append(executor.submit(shard.setup))

            # Wait for the setup of all nodes to complete
            for task in as_completed(tasks):
                task.result()

        # Need to get the new config shard connection string generated from the auto-bootstrap procedure
        if self.use_auto_bootstrap_procedure:
            for mongos in self.mongos:
                mongos.mongos_options["configdb"] = self.configsvr.get_internal_connection_string()

        if self.launch_mongot:
            # These mongot parameters are popped from shard.mongod_options when mongod is launched in above
            # setup() call. As such, the final values can't be cleanly copied over from mongod_options, but
            # need to be recreated here.
            if self.mongos[0].mongos_options["set_parameters"].get("useGrpcForSearch"):
                # If mongos & mongod are configured to use egress gRPC for search, then set the
                # `mongotHost` parameter to the mongot listening address expecting communication via
                # the MongoDB gRPC protocol (which we configured in setup_mongot_params).
                self.mongotHost = "localhost:" + str(self.shards[-1].mongot_grpc_port)
            else:
                self.mongotHost = "localhost:" + str(self.shards[-1].mongot_port)

            self.searchIndexManagementHostAndPort = self.mongotHost

            for mongos in self.mongos:
                # In search enabled sharded cluster, mongos has to be spun up with a connection string to a
                # mongot in order to issue PlanShardedSearch commands.
                mongos.mongos_options["mongotHost"] = self.mongotHost
                mongos.mongos_options["searchIndexManagementHostAndPort"] = (
                    self.searchIndexManagementHostAndPort
                )

        with ThreadPoolExecutor() as executor:
            tasks = []
            for mongos in self.mongos:
                tasks.append(executor.submit(mongos.setup))

            # Wait for the setup of all nodes to complete
            for task in as_completed(tasks):
                task.result()

    def _all_mongo_d_s_t(self):
        """Return a list of all `mongo{d,s,t}` `Process` instances in this fixture."""
        # When config_shard is None, we have an additional replset for the configsvr.
        all_nodes = [self.configsvr] if self.config_shard is None else []
        all_nodes += self.mongos
        all_nodes += self.shards
        return sum([node._all_mongo_d_s_t() for node in all_nodes], [])

    def _all_mongots(self):
        """Return a list of all `mongot` `Process` instances in this fixture."""
        mongot_processes = []
        for replicaset in self.shards:
            mongot_processes.extend(replicaset._all_mongots())
        return mongot_processes

    def refresh_logical_session_cache(self, target):
        """Refresh logical session cache with no timeout."""
        primary = target.get_primary().mongo_client()
        try:
            primary.admin.command({"refreshLogicalSessionCacheNow": 1})
        except pymongo.errors.OperationFailure as err:
            if err.code != self._WRITE_CONCERN_FAILED:
                raise err
            self.logger.info(
                "Ignoring write concern timeout for refreshLogicalSessionCacheNow "
                "command and continuing to wait"
            )
            target.await_last_op_committed(target.AWAIT_REPL_TIMEOUT_FOREVER_MINS * 60)

    def get_shard_ids(self):
        """Get the list of shard ids in the cluster."""
        client = interface.build_client(self, self.auth_options)
        res = client.admin.command("listShards")
        return [shard_info["_id"] for shard_info in res["shards"]]

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        with ThreadPoolExecutor() as executor:
            tasks = []

            # Wait for the config server
            if self.configsvr is not None and self.config_shard is None:
                tasks.append(executor.submit(self.configsvr.await_ready))

            # Wait for each of the shards
            for shard in self.shards:
                tasks.append(executor.submit(shard.await_ready))

            for mongos in self.mongos:
                tasks.append(executor.submit(mongos.await_ready))

            # Wait for all the nodes to be ready
            for task in as_completed(tasks):
                task.result()

        client = interface.build_client(self, self.auth_options)

        # Turn off the balancer if it is not meant to be enabled.
        if not self.enable_balancer:
            self.stop_balancer(join_migrations=False)

        # Inform mongos about each of the shards
        for idx, shard in enumerate(self.shards):
            self._add_shard(client, shard, self.config_shard == idx)

        # Ensure that all CSRS nodes are up to date. This is strictly needed for tests that use
        # multiple mongoses. In those cases, the first mongos initializes the contents of the config
        # database, but without waiting for those writes to replicate to all the config servers then
        # the secondary mongoses risk reading from a stale config server and seeing an empty config
        # database.
        self.configsvr.await_last_op_committed()

        # Ensure that the sessions collection gets auto-sharded by the config server
        if self.configsvr is not None:
            primary_mongo_client = self.configsvr.get_primary().mongo_client()
            refresh_logical_session_cache_with_retry(primary_mongo_client, self.configsvr)

        for shard in self.shards:
            self.refresh_logical_session_cache(shard)

        if self.set_cluster_parameter:
            self.run_set_cluster_parameter()

        if self.inject_catalog_metadata:
            csrs_client = interface.build_client(self.configsvr, self.auth_options)
            inject_catalog_metadata_on_the_csrs(csrs_client, self.inject_catalog_metadata)

        self.is_ready = True

    def run_set_cluster_parameter(self):
        """Set a cluster parameter for the fixture."""
        client = interface.build_client(self, self.auth_options)
        command_request = {
            "setClusterParameter": {
                self.set_cluster_parameter["parameter"]: self.set_cluster_parameter["value"]
            },
        }
        client.admin.command(command_request)

        # Make sure all mongos are aware of the new parameter values before tests are ran. This is
        # needed because mongos only refresh their cache of cluster parameters periodically.
        # Running getClusterParameter on a router causes it to refresh its cache.
        for mongos in self.mongos:
            mongos.mongo_client().admin.command(
                {"getClusterParameter": self.set_cluster_parameter["parameter"]}
            )

    def stop_balancer(self, timeout_ms=300000, join_migrations=True):
        """Stop the balancer."""
        client = interface.build_client(self, self.auth_options)
        client.admin.command({"balancerStop": 1}, maxTimeMS=timeout_ms)
        if join_migrations:
            for shard in self.shards:
                shard_client = interface.build_client(shard.get_primary(), self.auth_options)
                shard_client.admin.command({"_shardsvrJoinMigrations": 1})
        self.logger.info("Stopped the balancer")

    def start_balancer(self, timeout_ms=300000):
        """Start the balancer."""
        client = interface.build_client(self, self.auth_options)
        client.admin.command({"balancerStart": 1}, maxTimeMS=timeout_ms)
        self.logger.info("Started the balancer")

    def feature_flag_present_and_enabled(self, feature_flag_name):
        full_ff_name = f"featureFlag{feature_flag_name}"
        csrs_client = interface.build_client(self.configsvr, self.auth_options)
        try:
            res = csrs_client.admin.command({"getParameter": 1, full_ff_name: 1})
            return bool(res[full_ff_name]["value"])
        except pymongo.errors.OperationFailure as err:
            if err.code == 72:  # InvalidOptions
                # The feature flag is not present
                return False
            raise err

    def get_shard_object(self, conn_string):
        """Return the shard object that matches the shard referenced in conn_string."""
        hosts = conn_string.split("/")[1].split(",")
        ports = []
        for host in hosts:
            ports += [host.split(":")[1]]

        for shard in self.shards:
            shard_info = shard.get_node_info()
            for node in shard_info:
                if str(node.port) in ports:
                    return shard

    def _do_teardown(self, finished=False, mode=None):
        """Shut down the sharded cluster."""
        self.logger.info("Stopping all members of the sharded cluster...")

        if finished and self.load_all_extensions and self.loaded_extensions:
            delete_extension_configs(self.loaded_extensions, self.logger)

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.warning(
                "All members of the sharded cluster were expected to be running, " "but weren't."
            )

        # If we're killing or aborting to archive data files, stopping the balancer will execute
        # server commands that might lead to on-disk changes from the point of failure.
        if self.enable_balancer and mode not in (
            interface.TeardownMode.KILL,
            interface.TeardownMode.ABORT,
        ):
            self.stop_balancer()

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        with ThreadPoolExecutor() as executor:
            tasks = []
            for mongos in self.mongos:
                tasks.append(executor.submit(teardown_handler.teardown, mongos, "mongos", mode))
            # Wait for all mongod teardown tasks to complete
            for task in as_completed(tasks):
                task.result()

            tasks = []
            for shard in self.shards:
                if shard is self.configsvr:
                    continue
                tasks.append(executor.submit(teardown_handler.teardown, shard, "shard", mode))

            if self.configsvr is not None:
                tasks.append(
                    executor.submit(
                        teardown_handler.teardown, self.configsvr, "config server", mode
                    )
                )
            # Wait for all mongod teardown tasks to complete
            for task in as_completed(tasks):
                task.result()

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped all members of the sharded cluster.")
        else:
            self.logger.error("Stopping the sharded cluster fixture failed.")
            raise self.fixturelib.ServerFailure(teardown_handler.get_error_message())

        self.is_ready = False

    def is_running(self):
        """Return true if all nodes in the cluster are all still operating."""
        return (
            self.configsvr is not None
            and self.configsvr.is_running()
            and all(
                shard.is_running() for shard in self.shards if not shard.removeshard_teardown_marker
            )
            and all(mongos.is_running() for mongos in self.mongos)
        )

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        if self.mongos is None:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")

        return ",".join([mongos.get_internal_connection_string() for mongos in self.mongos])

    def get_shell_connection_string(self, use_grpc=False):
        if self.mongos is None:
            raise ValueError("Must call setup() before calling get_shell_connection_string()")

        return ",".join([mongos.get_shell_connection_string(use_grpc) for mongos in self.mongos])

    def _get_replica_set_endpoint(self):
        # The replica set endpoint would only become active after the replica set has become a
        # config shard (i.e. after the addShard or transitionFromConfigServer step) so before
        # that we must connect to a mongos or the router port of a mongod to run sharding
        # commands.
        if len(self.shards) == 0:
            raise ValueError(
                "Must call install_rs_shard() before calling get_internal_connection_string()"
            )
        if len(self.shards) > 1:
            raise ValueError("Cannot use replica set endpoint on a multi-shard cluster")
        return self.shards[0]

    def get_shell_connection_url(self):
        """Return the driver connection URL."""
        if self.is_ready and self.replica_endpoint_mode:
            return self._get_replica_set_endpoint().get_shell_connection_url()

        return "mongodb://" + self.get_shell_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        if self.is_ready and self.replica_endpoint_mode:
            return self._get_replica_set_endpoint().get_driver_connection_url()

        return "mongodb://" + self.get_internal_connection_string()

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        for shard in self.shards:
            if shard is self.configsvr:
                continue
            output += shard.get_node_info()
        for mongos in self.mongos:
            output += mongos.get_node_info()
        return output + self.configsvr.get_node_info()

    def get_configsvr_logger(self):
        """Return a new logging.Logger instance used for a config server shard."""
        return self.fixturelib.new_fixture_node_logger(
            self.__class__.__name__, self.job_num, self.configsvr_shard_logging_prefix
        )

    def get_configsvr_kwargs(self):
        """Return args to create replicaset.ReplicaSetFixture configured as the config server."""
        configsvr_options = self.configsvr_options.copy()
        auth_options = configsvr_options.pop("auth_options", self.auth_options)
        preserve_dbpath = configsvr_options.pop("preserve_dbpath", self.preserve_dbpath)
        num_nodes = configsvr_options.pop("num_nodes", 1)

        replset_config_options = configsvr_options.pop("replset_config_options", {})
        replset_config_options["configsvr"] = True

        mongod_options = self.mongod_options.copy()
        mongod_options = self.fixturelib.merge_mongo_option_dicts(
            mongod_options,
            self.fixturelib.make_historic(configsvr_options.pop("mongod_options", {})),
        )
        mongod_options["configsvr"] = ""
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "config")
        mongod_options["replSet"] = self.configsvr_replset_name
        mongod_options["storageEngine"] = "wiredTiger"

        return {
            "mongod_options": mongod_options,
            "mongod_executable": self.mongod_executable,
            "preserve_dbpath": preserve_dbpath,
            "num_nodes": num_nodes,
            "auth_options": auth_options,
            "replset_config_options": replset_config_options,
            "shard_logging_prefix": self.configsvr_shard_logging_prefix,
            **configsvr_options,
        }

    def install_configsvr(self, configsvr):
        """Install a configsvr. Called by a builder."""
        self.configsvr = configsvr

    def _get_rs_shard_logging_prefix(self, index):
        """Return replica set shard logging prefix."""
        return f"{self.rs_shard_logging_prefix}{index}"

    def get_rs_shard_logger(self, index):
        """Return a new logging.Logger instance used for a replica set shard."""
        shard_logging_prefix = self._get_rs_shard_logging_prefix(index)
        return self.fixturelib.new_fixture_node_logger(
            self.__class__.__name__, self.job_num, shard_logging_prefix
        )

    def get_rs_shard_kwargs(self, index):
        """Return args to create replicaset.ReplicaSetFixture configured as a shard in a sharded cluster."""
        shard_options = self.shard_options.copy()

        auth_options = shard_options.pop("auth_options", self.auth_options)
        preserve_dbpath = shard_options.pop("preserve_dbpath", self.preserve_dbpath)

        replset_config_options = shard_options.pop("replset_config_options", {})
        replset_config_options = replset_config_options.copy()
        replset_config_options["configsvr"] = False

        mongod_options = self.mongod_options.copy()
        mongod_options = self.fixturelib.merge_mongo_option_dicts(
            mongod_options, self.fixturelib.make_historic(shard_options.pop("mongod_options", {}))
        )
        mongod_options["shardsvr"] = ""
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "shard{}".format(index))
        mongod_options["replSet"] = self.shard_replset_name_prefix + str(index)

        if self.config_shard == index:
            del mongod_options["shardsvr"]
            mongod_options["configsvr"] = ""
            replset_config_options["configsvr"] = True
            mongod_options["storageEngine"] = "wiredTiger"

            configsvr_options = self.configsvr_options.copy()

            if "mongod_options" in configsvr_options:
                mongod_options = self.fixturelib.merge_mongo_option_dicts(
                    mongod_options, configsvr_options["mongod_options"]
                )
            if "replset_config_options" in configsvr_options:
                replset_config_options = self.fixturelib.merge_mongo_option_dicts(
                    replset_config_options, configsvr_options["replset_config_options"]
                )

            for option, value in configsvr_options.items():
                if option in ("num_nodes", "mongod_options", "replset_config_options"):
                    continue
                if option in shard_options:
                    if shard_options[option] != value:
                        raise Exception(
                            "Conflicting values when combining shard and configsvr options"
                        )
                else:
                    shard_options[option] = value

        use_auto_bootstrap_procedure = (
            self.use_auto_bootstrap_procedure and self.config_shard == index
        )
        shard_logging_prefix = self._get_rs_shard_logging_prefix(index)

        return {
            "mongod_options": mongod_options,
            "mongod_executable": self.mongod_executable,
            "auth_options": auth_options,
            "preserve_dbpath": preserve_dbpath,
            "replset_config_options": replset_config_options,
            "shard_logging_prefix": shard_logging_prefix,
            "use_auto_bootstrap_procedure": use_auto_bootstrap_procedure,
            **shard_options,
        }

    def install_rs_shard(self, rs_shard):
        """Install a replica set shard. Called by a builder."""
        self.shards.append(rs_shard)

    def get_mongos_logger(self, index, total):
        """Return a new logging.Logger instance used for a mongos."""
        logger_name = (
            self.mongos_logging_prefix if total == 1 else f"{self.mongos_logging_prefix}{index}"
        )
        return self.fixturelib.new_fixture_node_logger(
            self.__class__.__name__, self.job_num, logger_name
        )

    def get_mongos_kwargs(self):
        """Return options that may be passed to a mongos."""
        mongos_options = self.mongos_options.copy()
        mongos_options["configdb"] = self.configsvr.get_internal_connection_string()
        if self.config_shard is not None:
            if "set_parameters" not in mongos_options:
                mongos_options["set_parameters"] = {}
        mongos_options["set_parameters"] = mongos_options.get(
            "set_parameters", self.fixturelib.make_historic({})
        ).copy()
        return {"dbpath_prefix": self._dbpath_prefix, "mongos_options": mongos_options}

    def install_mongos(self, mongos):
        """Install a mongos. Called by a builder."""
        self.mongos.append(mongos)

    def _add_shard(self, client, shard, is_config_shard):
        """
        Add the specified program as a shard by executing the addShard command.

        See https://docs.mongodb.org/manual/reference/command/addShard for more details.
        """
        connection_string = shard.get_internal_connection_string()
        if is_config_shard:
            if not self.use_auto_bootstrap_procedure:
                self.logger.info("Adding %s as config shard...", connection_string)
                client.admin.command({"transitionFromDedicatedConfigServer": 1})
        else:
            self.logger.info("Adding %s as a shard...", connection_string)
            client.admin.command({"addShard": connection_string})


class ExternalShardedClusterFixture(external.ExternalFixture, ShardedClusterFixture):
    """Fixture to interact with external sharded cluster fixture."""

    REGISTERED_NAME = "ExternalShardedClusterFixture"

    def __init__(self, logger, job_num, fixturelib, original_suite_name):
        """Initialize ExternalShardedClusterFixture."""
        self.dummy_fixture = _builder.make_dummy_fixture(original_suite_name)
        self.shell_conn_string = "mongodb://" + ",".join(
            [f"mongos{i}:27017" for i in range(self.dummy_fixture.num_mongos)]
        )

        external.ExternalFixture.__init__(self, logger, job_num, fixturelib, self.shell_conn_string)
        ShardedClusterFixture.__init__(self, logger, job_num, fixturelib, mongod_options={})

    def setup(self):
        """Execute some setup before offically starting testing against this external cluster."""
        client = pymongo.MongoClient(self.get_driver_connection_url())
        for i in range(50):
            if i == 49:
                raise RuntimeError("Sharded Cluster setup has timed out.")
            payload = client.admin.command({"listShards": 1})
            if len(payload["shards"]) == self.dummy_fixture.num_shards:
                print("Sharded Cluster available.")
                break
            if len(payload["shards"]) < self.dummy_fixture.num_shards:
                print("Waiting for shards to be added to cluster.")
                time.sleep(5)
                continue
            if len(payload["shards"]) > self.dummy_fixture.num_shards:
                raise RuntimeError("More shards in cluster than expected.")

    def pids(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.pids(self)

    def await_ready(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.await_ready(self)

    def _do_teardown(self, finished=False, mode=None):
        """Use ExternalFixture method."""
        return external.ExternalFixture._do_teardown(self)

    def _is_process_running(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture._is_process_running(self)

    def is_running(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.is_running(self)

    def get_internal_connection_string(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.get_internal_connection_string(self)

    def get_driver_connection_url(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.get_driver_connection_url(self)

    def get_shell_connection_url(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.get_shell_connection_url(self)

    def get_node_info(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.get_node_info(self)


class _MongoSFixture(interface.Fixture, interface._DockerComposeInterface):
    """Fixture which provides JSTests with a mongos to connect to."""

    def __init__(
        self,
        logger,
        job_num,
        fixturelib,
        dbpath_prefix,
        mongos_executable=None,
        mongos_options=None,
        add_feature_flags=False,
    ):
        """Initialize _MongoSFixture."""

        interface.Fixture.__init__(self, logger, job_num, fixturelib)

        self.fixturelib = fixturelib
        self.config = self.fixturelib.get_config()

        # Default to command line options if the YAML configuration is not passed in.
        self.mongos_executable = self.fixturelib.default_if_none(
            mongos_executable, self.config.MONGOS_EXECUTABLE
        )

        self.mongos_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(mongos_options, {})
        ).copy()

        if add_feature_flags:
            for ff in self.config.ENABLED_FEATURE_FLAGS:
                self.mongos_options["set_parameters"][ff] = "true"

        self.mongos = None
        self.port = fixturelib.get_next_port(job_num)
        self.mongos_options["port"] = self.port
        if "featureFlagGRPC" in self.config.ENABLED_FEATURE_FLAGS:
            self.grpcPort = fixturelib.get_next_port(job_num)
            self.mongos_options["grpcPort"] = self.grpcPort

        self._dbpath_prefix = dbpath_prefix

    def setup(self):
        """Set up the sharded cluster."""
        if self.config.ALWAYS_USE_LOG_FILES:
            self.mongos_options["logpath"] = self._dbpath_prefix + "/{name}.log".format(
                name=self.logger.name
            )
            self.mongos_options["logappend"] = ""

        launcher = MongosLauncher(self.fixturelib)
        mongos, _ = launcher.launch_mongos_program(
            self.logger,
            self.job_num,
            executable=self.mongos_executable,
            mongos_options=self.mongos_options,
        )
        self.mongos_options["port"] = self.port
        try:
            self.logger.info("Starting mongos on port %d...\n%s", self.port, mongos.as_command())
            mongos.start()
            self.logger.info("mongos started on port %d with pid %d.", self.port, mongos.pid)
        except Exception as err:
            msg = "Failed to start mongos on port {:d}: {}".format(self.port, err)
            self.logger.exception(msg)
            raise self.fixturelib.ServerFailure(msg)

        self.mongos = mongos

    def _all_mongo_d_s_t(self):
        """Return the standalone `mongos` `Process` instance."""
        return [self]

    def pids(self):
        """:return: pids owned by this fixture if any."""
        if self.mongos is not None:
            return [self.mongos.pid]
        else:
            self.logger.debug("Mongos not running when gathering mongos fixture pids.")
        return []

    def is_from_configsvr(self):
        """Return false, because this router is not part of a mongod."""
        return False

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        deadline = time.time() + interface.Fixture.AWAIT_READY_TIMEOUT_SECS

        # Wait until the mongos is accepting connections. The retry logic is necessary to support
        # versions of PyMongo <3.0 that immediately raise a ConnectionFailure if a connection cannot
        # be established.
        while True:
            # Check whether the mongos exited for some reason.
            exit_code = self.mongos.poll()
            if exit_code is not None:
                raise self.fixturelib.ServerFailure(
                    "Could not connect to mongos on port {}, process ended"
                    " unexpectedly with code {}.".format(self.port, exit_code)
                )

            try:
                # Use a shorter connection timeout to more closely satisfy the requested deadline.
                client = self.mongo_client(timeout_millis=500)
                client.admin.command("ping")
                break
            except pymongo.errors.ConnectionFailure:
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    raise self.fixturelib.ServerFailure(
                        "Failed to connect to mongos on port {} after {} seconds".format(
                            self.port, interface.Fixture.AWAIT_READY_TIMEOUT_SECS
                        )
                    )

                self.logger.info("Waiting to connect to mongos on port %d.", self.port)
                time.sleep(0.1)  # Wait a little bit before trying again.

        self.logger.info("Successfully contacted the mongos on port %d.", self.port)

    def _do_teardown(self, finished=False, mode=None):
        if self.config.NOOP_MONGO_D_S_PROCESSES:
            self.logger.info(
                "This is running against an External System Under Test setup with `docker-compose.yml` -- skipping teardown."
            )
            return

        if self.mongos is None:
            self.logger.warning("The mongos fixture has not been set up yet.")
            return  # Teardown is still a success even if nothing is running.

        if mode == interface.TeardownMode.ABORT:
            self.logger.info(
                "Attempting to send SIGABRT from resmoke to mongos on port %d with pid %d...",
                self.port,
                self.mongos.pid,
            )
        else:
            self.logger.info(
                "Stopping mongos on port %d with pid %d...", self.port, self.mongos.pid
            )
        if not self.is_running():
            exit_code = self.mongos.poll()
            msg = (
                "mongos on port {:d} was expected to be running, but wasn't. "
                "Process exited with code {:d}"
            ).format(self.port, exit_code)
            self.logger.warning(msg)
            raise self.fixturelib.ServerFailure(msg)

        self.mongos.stop(mode=mode)
        exit_code = self.mongos.wait()

        # Python's subprocess module returns negative versions of system calls.
        if exit_code == 0 or (mode is not None and exit_code == -(mode.value)):
            self.logger.info("Successfully stopped the mongos on port {:d}".format(self.port))
        else:
            self.logger.warning(
                "Stopped the mongos on port {:d}. " "Process exited with code {:d}.".format(
                    self.port, exit_code
                )
            )
            raise self.fixturelib.ServerFailure(
                "mongos on port {:d} with pid {:d} exited with code {:d}".format(
                    self.port, self.mongos.pid, exit_code
                )
            )

    def is_running(self):
        """Return true if the cluster is still operating."""
        return self.mongos is not None and self.mongos.poll() is None

    def _get_hostname(self):
        return (
            self.logger.external_sut_hostname
            if self.config.NOOP_MONGO_D_S_PROCESSES
            else "localhost"
        )

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        return f"{self._get_hostname()}:{self.port}"

    def get_shell_connection_string(self, use_grpc=False):
        port = self.port if not (self.config.SHELL_GRPC or use_grpc) else self.grpcPort
        return f"{self._get_hostname()}:{port}"

    def get_shell_connection_url(self):
        return "mongodb://" + self.get_shell_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return "mongodb://" + self.get_internal_connection_string()

    def get_node_info(self):
        """Return a list of NodeInfo objects."""
        if self.mongos is None:
            self.logger.warning("The mongos fixture has not been set up yet.")
            return []

        info = interface.NodeInfo(
            full_name=self.logger.full_name,
            name=self.logger.name,
            port=self.port,
            pid=self.mongos.pid,
        )
        return [info]


# Default shutdown quiesce mode duration for mongos
DEFAULT_MONGOS_SHUTDOWN_TIMEOUT_MILLIS = 0

# The default verbosity setting for any tests that are not started with an Evergreen task id. This
# will apply to any tests run locally.
DEFAULT_MONGOS_LOG_COMPONENT_VERBOSITY = {"transaction": 3}

# The default verbosity setting for any tests running in Evergreen i.e. started with an Evergreen
# task id.
DEFAULT_EVERGREEN_MONGOS_LOG_COMPONENT_VERBOSITY = {"transaction": 3}


class MongosLauncher(object):
    """Class with utilities for launching a mongos."""

    def __init__(self, fixturelib):
        """Initialize MongosLauncher."""
        self.fixturelib = fixturelib
        self.config = fixturelib.get_config()

    def default_mongos_log_component_verbosity(self):
        """Return the default 'logComponentVerbosity' value to use for mongos processes."""
        if self.config.EVERGREEN_TASK_ID:
            return DEFAULT_EVERGREEN_MONGOS_LOG_COMPONENT_VERBOSITY
        return DEFAULT_MONGOS_LOG_COMPONENT_VERBOSITY

    def launch_mongos_program(
        self, logger, job_num, executable=None, process_kwargs=None, mongos_options=None
    ):
        """Return a Process instance that starts a mongos with arguments constructed from 'kwargs'."""

        executable = self.fixturelib.default_if_none(
            executable, self.config.DEFAULT_MONGOS_EXECUTABLE
        )

        # Apply the --setParameter command line argument. Command line options to resmoke.py override
        # the YAML configuration.
        suite_set_parameters = mongos_options.setdefault("set_parameters", {})

        if self.config.MONGOS_SET_PARAMETERS is not None:
            suite_set_parameters.update(yaml.safe_load(self.config.MONGOS_SET_PARAMETERS))

        if "mongotHost" in mongos_options:
            suite_set_parameters["mongotHost"] = mongos_options.pop("mongotHost")
            suite_set_parameters["searchIndexManagementHostAndPort"] = mongos_options.pop(
                "searchIndexManagementHostAndPort"
            )

        # Use a higher timeout finding a host to avoid spurious failures on slow machines.
        if "defaultConfigCommandTimeoutMS" not in suite_set_parameters:
            suite_set_parameters["defaultConfigCommandTimeoutMS"] = 5 * 60 * 1000

        # Set default log verbosity levels if none were specified.
        if "logComponentVerbosity" not in suite_set_parameters:
            suite_set_parameters["logComponentVerbosity"] = (
                self.default_mongos_log_component_verbosity()
            )

        # Set default shutdown timeout millis if none was specified.
        if "mongosShutdownTimeoutMillisForSignaledShutdown" not in suite_set_parameters:
            suite_set_parameters["mongosShutdownTimeoutMillisForSignaledShutdown"] = (
                DEFAULT_MONGOS_SHUTDOWN_TIMEOUT_MILLIS
            )

        # If a JS_GC_ZEAL value has been provided in the configuration under MOZJS_JS_GC_ZEAL,
        # we inject this value directly as an environment variable to be passed to the spawned
        # mongos process.
        if self.config.MOZJS_JS_GC_ZEAL:
            process_kwargs = self.fixturelib.default_if_none(process_kwargs, {}).copy()
            env_vars = process_kwargs.setdefault("env_vars", {}).copy()
            env_vars.setdefault("JS_GC_ZEAL", self.config.MOZJS_JS_GC_ZEAL)
            process_kwargs["env_vars"] = env_vars

        _add_testing_set_parameters(suite_set_parameters)

        return self.fixturelib.mongos_program(
            logger, job_num, executable, process_kwargs, mongos_options
        )


def _add_testing_set_parameters(suite_set_parameters):
    """
    Add certain behaviors should only be enabled for resmoke usage.

    These are traditionally enable new commands, insecure access, and increased diagnostics.
    """
    suite_set_parameters.setdefault("testingDiagnosticsEnabled", True)
    suite_set_parameters.setdefault("enableTestCommands", True)
    suite_set_parameters.setdefault("disableTransitionFromLatestToLastContinuous", False)
