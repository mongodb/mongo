"""Sharded cluster fixture for executing JSTests against."""

import os.path
import time
import yaml

import pymongo
import pymongo.errors

import buildscripts.resmokelib.testing.fixtures.interface as interface
import buildscripts.resmokelib.testing.fixtures.external as external


class ShardedClusterFixture(interface.Fixture):
    """Fixture which provides JSTests with a sharded cluster to run against."""

    _CONFIGSVR_REPLSET_NAME = "config-rs"
    _SHARD_REPLSET_NAME_PREFIX = "shard-rs"

    AWAIT_SHARDING_INITIALIZATION_TIMEOUT_SECS = 60

    def __init__(self, logger, job_num, fixturelib, mongos_executable=None, mongos_options=None,
                 mongod_executable=None, mongod_options=None, dbpath_prefix=None,
                 preserve_dbpath=False, num_shards=1, num_rs_nodes_per_shard=1, num_mongos=1,
                 enable_sharding=None, enable_balancer=True, enable_autosplit=True,
                 auth_options=None, configsvr_options=None, shard_options=None,
                 cluster_logging_prefix=None):
        """Initialize ShardedClusterFixture with different options for the cluster processes."""

        interface.Fixture.__init__(self, logger, job_num, fixturelib, dbpath_prefix=dbpath_prefix)

        if "dbpath" in mongod_options:
            raise ValueError("Cannot specify mongod_options.dbpath")

        self.mongos_executable = mongos_executable
        self.mongos_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(mongos_options, {}))
        self.mongod_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(mongod_options, {}))
        self.mongod_executable = mongod_executable
        self.mongod_options["set_parameters"] = self.fixturelib.make_historic(
            mongod_options.get("set_parameters", {})).copy()
        self.mongod_options["set_parameters"]["migrationLockAcquisitionMaxWaitMS"] = \
                self.mongod_options["set_parameters"].get("migrationLockAcquisitionMaxWaitMS", 30000)
        self.preserve_dbpath = preserve_dbpath
        self.num_shards = num_shards
        self.num_rs_nodes_per_shard = num_rs_nodes_per_shard
        self.num_mongos = num_mongos
        self.enable_sharding = self.fixturelib.default_if_none(enable_sharding, [])
        self.enable_balancer = enable_balancer
        self.enable_autosplit = enable_autosplit
        self.auth_options = auth_options
        self.configsvr_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(configsvr_options, {}))
        self.shard_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(shard_options, {}))

        # The logging prefix used in cluster to cluster replication.
        self.cluster_logging_prefix = "" if cluster_logging_prefix is None else f"{cluster_logging_prefix}:"

        self.configsvr_shard_logging_prefix = f"{self.cluster_logging_prefix}configsvr"
        self.rs_shard_logging_prefix = f"{self.cluster_logging_prefix}shard"
        self.mongos_logging_prefix = f"{self.cluster_logging_prefix}mongos"

        if self.num_rs_nodes_per_shard is None:
            raise TypeError("num_rs_nodes_per_shard must be an integer but found None")
        elif isinstance(self.num_rs_nodes_per_shard, int):
            if self.num_rs_nodes_per_shard <= 0:
                raise ValueError("num_rs_nodes_per_shard must be a positive integer")

        self._dbpath_prefix = os.path.join(self._dbpath_prefix, self.config.FIXTURE_SUBDIR)

        self.configsvr = None
        self.mongos = []
        self.shards = []

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = []
        if self.configsvr is not None:
            out.extend(self.configsvr.pids())
        else:
            self.logger.debug(
                'Config server not running when gathering sharded cluster fixture pids.')
        if self.shards is not None:
            for shard in self.shards:
                out.extend(shard.pids())
        else:
            self.logger.debug('No shards when gathering sharded cluster fixture pids.')
        return out

    def setup(self):
        """Set up the sharded cluster."""
        self.configsvr.setup()

        # Start up each of the shards
        for shard in self.shards:
            shard.setup()

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

    def get_shard_ids(self):
        """Get the list of shard ids in the cluster."""
        client = self.mongo_client()
        interface.authenticate(client, self.auth_options)
        res = client.admin.command("listShards")
        return [shard_info["_id"] for shard_info in res["shards"]]

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        # Wait for the config server
        if self.configsvr is not None:
            self.configsvr.await_ready()

        # Wait for each of the shards
        for shard in self.shards:
            shard.await_ready()

        # We call mongos.setup() in self.await_ready() function instead of self.setup()
        # because mongos routers have to connect to a running cluster.
        for mongos in self.mongos:
            # Start up the mongos.
            mongos.setup()

            # Wait for the mongos.
            mongos.await_ready()

        client = self.mongo_client()
        interface.authenticate(client, self.auth_options)

        # Turn off the balancer if it is not meant to be enabled.
        if not self.enable_balancer:
            self.stop_balancer()

        # Turn off autosplit if it is not meant to be enabled.
        if not self.enable_autosplit:
            wc = pymongo.WriteConcern(w="majority", wtimeout=30000)
            coll = client.config.get_collection("settings", write_concern=wc)
            coll.update_one({"_id": "autosplit"}, {"$set": {"enabled": False}}, upsert=True)

        # Inform mongos about each of the shards
        for shard in self.shards:
            self._add_shard(client, shard)

        # Ensure that all CSRS nodes are up to date. This is strictly needed for tests that use
        # multiple mongoses. In those cases, the first mongos initializes the contents of the config
        # database, but without waiting for those writes to replicate to all the config servers then
        # the secondary mongoses risk reading from a stale config server and seeing an empty config
        # database.
        self.configsvr.await_last_op_committed()

        # Enable sharding on each of the specified databases
        for db_name in self.enable_sharding:
            self.logger.info("Enabling sharding for '%s' database...", db_name)
            client.admin.command({"enablesharding": db_name})

        # Wait for mongod's to be ready.
        self._await_mongod_sharding_initialization()

        # Ensure that the sessions collection gets auto-sharded by the config server
        if self.configsvr is not None:
            self.refresh_logical_session_cache(self.configsvr)

        for shard in self.shards:
            self.refresh_logical_session_cache(shard)

    def _await_mongod_sharding_initialization(self):
        if (self.enable_sharding) and (self.num_rs_nodes_per_shard is not None):
            deadline = time.time(
            ) + ShardedClusterFixture.AWAIT_SHARDING_INITIALIZATION_TIMEOUT_SECS
            timeout_occurred = lambda: deadline - time.time() <= 0.0

            mongod_clients = [(mongod.mongo_client(), mongod.port) for shard in self.shards
                              for mongod in shard.nodes]

            for client, port in mongod_clients:
                interface.authenticate(client, self.auth_options)

                while True:
                    # The choice of namespace (local.fooCollection) does not affect the output.
                    get_shard_version_result = client.admin.command(
                        "getShardVersion", "local.fooCollection", check=False)
                    if get_shard_version_result["ok"]:
                        break

                    if timeout_occurred():
                        raise self.fixturelib.ServerFailure(
                            "mongod on port: {} failed waiting for getShardVersion success after {} seconds"
                            .format(port, interface.Fixture.AWAIT_READY_TIMEOUT_SECS))
                    time.sleep(0.1)

    def stop_balancer(self, timeout_ms=60000):
        """Stop the balancer."""
        client = self.mongo_client()
        interface.authenticate(client, self.auth_options)
        client.admin.command({"balancerStop": 1}, maxTimeMS=timeout_ms)
        self.logger.info("Stopped the balancer")

    def start_balancer(self, timeout_ms=60000):
        """Start the balancer."""
        client = self.mongo_client()
        interface.authenticate(client, self.auth_options)
        client.admin.command({"balancerStart": 1}, maxTimeMS=timeout_ms)
        self.logger.info("Started the balancer")

    def _do_teardown(self, mode=None):
        """Shut down the sharded cluster."""
        self.logger.info("Stopping all members of the sharded cluster...")

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.warning("All members of the sharded cluster were expected to be running, "
                                "but weren't.")

        # If we're killing or aborting to archive data files, stopping the balancer will execute
        # server commands that might lead to on-disk changes from the point of failure.
        if self.enable_balancer and mode not in (interface.TeardownMode.KILL,
                                                 interface.TeardownMode.ABORT):
            self.stop_balancer()

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        for mongos in self.mongos:
            teardown_handler.teardown(mongos, "mongos", mode=mode)

        for shard in self.shards:
            teardown_handler.teardown(shard, "shard", mode=mode)

        if self.configsvr is not None:
            teardown_handler.teardown(self.configsvr, "config server", mode=mode)

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped all members of the sharded cluster.")
        else:
            self.logger.error("Stopping the sharded cluster fixture failed.")
            raise self.fixturelib.ServerFailure(teardown_handler.get_error_message())

    def is_running(self):
        """Return true if all nodes in the cluster are all still operating."""
        return (self.configsvr is not None and self.configsvr.is_running()
                and all(shard.is_running() for shard in self.shards)
                and all(mongos.is_running() for mongos in self.mongos))

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        if self.mongos is None:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")

        return ",".join([mongos.get_internal_connection_string() for mongos in self.mongos])

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return "mongodb://" + self.get_internal_connection_string()

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        for shard in self.shards:
            output += shard.get_node_info()
        for mongos in self.mongos:
            output += mongos.get_node_info()
        return output + self.configsvr.get_node_info()

    def get_configsvr_logger(self):
        """Return a new logging.Logger instance used for a config server shard."""
        return self.fixturelib.new_fixture_node_logger(self.__class__.__name__, self.job_num,
                                                       self.configsvr_shard_logging_prefix)

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
            self.fixturelib.make_historic(configsvr_options.pop("mongod_options", {})))
        mongod_options["configsvr"] = ""
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "config")
        mongod_options["replSet"] = ShardedClusterFixture._CONFIGSVR_REPLSET_NAME
        mongod_options["storageEngine"] = "wiredTiger"

        return {
            "mongod_options": mongod_options, "mongod_executable": self.mongod_executable,
            "preserve_dbpath": preserve_dbpath, "num_nodes": num_nodes,
            "auth_options": auth_options, "replset_config_options": replset_config_options,
            "shard_logging_prefix": self.configsvr_shard_logging_prefix, **configsvr_options
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
        return self.fixturelib.new_fixture_node_logger(self.__class__.__name__, self.job_num,
                                                       shard_logging_prefix)

    def get_rs_shard_kwargs(self, index):
        """Return args to create replicaset.ReplicaSetFixture configured as a shard in a sharded cluster."""
        shard_options = self.shard_options.copy()

        auth_options = shard_options.pop("auth_options", self.auth_options)
        preserve_dbpath = shard_options.pop("preserve_dbpath", self.preserve_dbpath)

        replset_config_options = self.fixturelib.make_historic(
            shard_options.pop("replset_config_options", {}))
        replset_config_options["configsvr"] = False

        mongod_options = self.mongod_options.copy()
        mongod_options = self.fixturelib.merge_mongo_option_dicts(
            mongod_options, self.fixturelib.make_historic(shard_options.pop("mongod_options", {})))
        mongod_options["shardsvr"] = ""
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "shard{}".format(index))
        mongod_options["replSet"] = self._SHARD_REPLSET_NAME_PREFIX + str(index)

        shard_logging_prefix = self._get_rs_shard_logging_prefix(index)

        return {
            "mongod_options": mongod_options, "mongod_executable": self.mongod_executable,
            "auth_options": auth_options, "preserve_dbpath": preserve_dbpath,
            "replset_config_options": replset_config_options,
            "shard_logging_prefix": shard_logging_prefix, **shard_options
        }

    def install_rs_shard(self, rs_shard):
        """Install a replica set shard. Called by a builder."""
        self.shards.append(rs_shard)

    def get_mongos_logger(self, index, total):
        """Return a new logging.Logger instance used for a mongos."""
        logger_name = self.mongos_logging_prefix if total == 1 else f"{self.mongos_logging_prefix}{index}"
        return self.fixturelib.new_fixture_node_logger(self.__class__.__name__, self.job_num,
                                                       logger_name)

    def get_mongos_kwargs(self):
        """Return options that may be passed to a mongos."""
        mongos_options = self.mongos_options.copy()
        mongos_options["configdb"] = self.configsvr.get_internal_connection_string()
        mongos_options["set_parameters"] = mongos_options.get("set_parameters",
                                                              self.fixturelib.make_historic(
                                                                  {})).copy()
        return {"dbpath_prefix": self._dbpath_prefix, "mongos_options": mongos_options}

    def install_mongos(self, mongos):
        """Install a mongos. Called by a builder."""
        self.mongos.append(mongos)

    def _add_shard(self, client, shard):
        """
        Add the specified program as a shard by executing the addShard command.

        See https://docs.mongodb.org/manual/reference/command/addShard for more details.
        """

        connection_string = shard.get_internal_connection_string()
        self.logger.info("Adding %s as a shard...", connection_string)
        client.admin.command({"addShard": connection_string})


class ExternalShardedClusterFixture(external.ExternalFixture, ShardedClusterFixture):
    """Fixture to interact with external sharded cluster fixture."""

    REGISTERED_NAME = "ExternalShardedClusterFixture"

    def __init__(self, logger, job_num, fixturelib, shell_conn_string):
        """Initialize ExternalShardedClusterFixture."""
        external.ExternalFixture.__init__(self, logger, job_num, fixturelib, shell_conn_string)
        ShardedClusterFixture.__init__(self, logger, job_num, fixturelib, mongod_options={})

    def setup(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.setup(self)

    def pids(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.pids(self)

    def await_ready(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.await_ready(self)

    def _do_teardown(self, mode=None):
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

    def get_node_info(self):
        """Use ExternalFixture method."""
        return external.ExternalFixture.get_node_info(self)


class _MongoSFixture(interface.Fixture):
    """Fixture which provides JSTests with a mongos to connect to."""

    def __init__(self, logger, job_num, fixturelib, dbpath_prefix, mongos_executable=None,
                 mongos_options=None, add_feature_flags=False):
        """Initialize _MongoSFixture."""

        interface.Fixture.__init__(self, logger, job_num, fixturelib)

        self.fixturelib = fixturelib
        self.config = self.fixturelib.get_config()

        # Default to command line options if the YAML configuration is not passed in.
        self.mongos_executable = self.fixturelib.default_if_none(mongos_executable,
                                                                 self.config.MONGOS_EXECUTABLE)

        self.mongos_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(mongos_options, {})).copy()

        if add_feature_flags:
            for ff in self.config.ENABLED_FEATURE_FLAGS:
                self.mongos_options["set_parameters"][ff] = "true"

        self.mongos = None
        self.port = fixturelib.get_next_port(job_num)
        self.mongos_options["port"] = self.port

        self._dbpath_prefix = dbpath_prefix

    def setup(self):
        """Set up the sharded cluster."""
        if self.config.ALWAYS_USE_LOG_FILES:
            self.mongos_options["logpath"] = self._dbpath_prefix + "/{name}.log".format(
                name=self.logger.name)
            self.mongos_options["logappend"] = ""

        launcher = MongosLauncher(self.fixturelib)
        mongos, _ = launcher.launch_mongos_program(self.logger, self.job_num,
                                                   executable=self.mongos_executable,
                                                   mongos_options=self.mongos_options)
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

    def pids(self):
        """:return: pids owned by this fixture if any."""
        if self.mongos is not None:
            return [self.mongos.pid]
        else:
            self.logger.debug('Mongos not running when gathering mongos fixture pids.')
        return []

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
                    " unexpectedly with code {}.".format(self.port, exit_code))

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
                            self.port, interface.Fixture.AWAIT_READY_TIMEOUT_SECS))

                self.logger.info("Waiting to connect to mongos on port %d.", self.port)
                time.sleep(0.1)  # Wait a little bit before trying again.

        self.logger.info("Successfully contacted the mongos on port %d.", self.port)

    def _do_teardown(self, mode=None):
        if self.mongos is None:
            self.logger.warning("The mongos fixture has not been set up yet.")
            return  # Teardown is still a success even if nothing is running.

        if mode == interface.TeardownMode.ABORT:
            self.logger.info(
                "Attempting to send SIGABRT from resmoke to mongos on port %d with pid %d...",
                self.port, self.mongos.pid)
        else:
            self.logger.info("Stopping mongos on port %d with pid %d...", self.port,
                             self.mongos.pid)
        if not self.is_running():
            exit_code = self.mongos.poll()
            msg = ("mongos on port {:d} was expected to be running, but wasn't. "
                   "Process exited with code {:d}").format(self.port, exit_code)
            self.logger.warning(msg)
            raise self.fixturelib.ServerFailure(msg)

        self.mongos.stop(mode=mode)
        exit_code = self.mongos.wait()

        # Python's subprocess module returns negative versions of system calls.
        if exit_code == 0 or (mode is not None and exit_code == -(mode.value)):
            self.logger.info("Successfully stopped the mongos on port {:d}".format(self.port))
        else:
            self.logger.warning("Stopped the mongos on port {:d}. "
                                "Process exited with code {:d}.".format(self.port, exit_code))
            raise self.fixturelib.ServerFailure(
                "mongos on port {:d} with pid {:d} exited with code {:d}".format(
                    self.port, self.mongos.pid, exit_code))

    def is_running(self):
        """Return true if the cluster is still operating."""
        return self.mongos is not None and self.mongos.poll() is None

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        return "localhost:%d" % self.port

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return "mongodb://" + self.get_internal_connection_string()

    def get_node_info(self):
        """Return a list of NodeInfo objects."""
        info = interface.NodeInfo(full_name=self.logger.full_name, name=self.logger.name,
                                  port=self.port, pid=self.mongos.pid)
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

    def launch_mongos_program(self, logger, job_num, executable=None, process_kwargs=None,
                              mongos_options=None):
        """Return a Process instance that starts a mongos with arguments constructed from 'kwargs'."""

        executable = self.fixturelib.default_if_none(executable,
                                                     self.config.DEFAULT_MONGOS_EXECUTABLE)

        # Apply the --setParameter command line argument. Command line options to resmoke.py override
        # the YAML configuration.
        suite_set_parameters = mongos_options.setdefault("set_parameters", {})

        if self.config.MONGOS_SET_PARAMETERS is not None:
            suite_set_parameters.update(yaml.safe_load(self.config.MONGOS_SET_PARAMETERS))

        # Set default log verbosity levels if none were specified.
        if "logComponentVerbosity" not in suite_set_parameters:
            suite_set_parameters[
                "logComponentVerbosity"] = self.default_mongos_log_component_verbosity()

        # Set default shutdown timeout millis if none was specified.
        if "mongosShutdownTimeoutMillisForSignaledShutdown" not in suite_set_parameters:
            suite_set_parameters[
                "mongosShutdownTimeoutMillisForSignaledShutdown"] = DEFAULT_MONGOS_SHUTDOWN_TIMEOUT_MILLIS

        _add_testing_set_parameters(suite_set_parameters)

        return self.fixturelib.mongos_program(logger, job_num, executable, process_kwargs,
                                              mongos_options)


def _add_testing_set_parameters(suite_set_parameters):
    """
    Add certain behaviors should only be enabled for resmoke usage.

    These are traditionally enable new commands, insecure access, and increased diagnostics.
    """
    suite_set_parameters.setdefault("testingDiagnosticsEnabled", True)
    suite_set_parameters.setdefault("enableTestCommands", True)
