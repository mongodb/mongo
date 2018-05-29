"""
Sharded cluster fixture for executing JSTests against.
"""

from __future__ import absolute_import

import os.path
import time

import pymongo
import pymongo.errors

from . import interface
from . import standalone
from . import replicaset
from ... import config
from ... import core
from ... import errors
from ... import utils
from ...utils import registry


class ShardedClusterFixture(interface.Fixture):
    """
    Fixture which provides JSTests with a sharded cluster to run
    against.
    """

    _CONFIGSVR_REPLSET_NAME = "config-rs"
    _SHARD_REPLSET_NAME_PREFIX = "shard-rs"

    def __init__(self,
                 logger,
                 job_num,
                 mongos_executable=None,
                 mongos_options=None,
                 mongod_executable=None,
                 mongod_options=None,
                 dbpath_prefix=None,
                 preserve_dbpath=False,
                 num_shards=1,
                 num_rs_nodes_per_shard=None,
                 separate_configsvr=True,
                 enable_sharding=None,
                 auth_options=None,
                 configsvr_options=None,
                 shard_options=None):
        """
        Initializes ShardedClusterFixture with the different options to
        the mongod and mongos processes.
        """

        interface.Fixture.__init__(self, logger, job_num, dbpath_prefix=dbpath_prefix)

        if "dbpath" in mongod_options:
            raise ValueError("Cannot specify mongod_options.dbpath")

        self.mongos_executable = mongos_executable
        self.mongos_options = utils.default_if_none(mongos_options, {})
        self.mongod_executable = mongod_executable
        self.mongod_options = utils.default_if_none(mongod_options, {})
        self.preserve_dbpath = preserve_dbpath
        self.num_shards = num_shards
        self.num_rs_nodes_per_shard = num_rs_nodes_per_shard
        self.separate_configsvr = separate_configsvr
        self.enable_sharding = utils.default_if_none(enable_sharding, [])
        self.auth_options = auth_options
        self.configsvr_options = utils.default_if_none(configsvr_options, {})
        self.shard_options = utils.default_if_none(shard_options, {})

        self._dbpath_prefix = os.path.join(self._dbpath_prefix, config.FIXTURE_SUBDIR)

        self.configsvr = None
        self.mongos = None
        self.shards = []

    def setup(self):
        if self.separate_configsvr:
            if self.configsvr is None:
                self.configsvr = self._new_configsvr()
            self.configsvr.setup()

        if not self.shards:
            for i in xrange(self.num_shards):
                if self.num_rs_nodes_per_shard is None:
                    shard = self._new_standalone_shard(i)
                elif isinstance(self.num_rs_nodes_per_shard, int):
                    if self.num_rs_nodes_per_shard <= 0:
                        raise ValueError("num_rs_nodes_per_shard must be a positive integer")
                    shard = self._new_rs_shard(i, self.num_rs_nodes_per_shard)
                else:
                    raise TypeError("num_rs_nodes_per_shard must be an integer or None")
                self.shards.append(shard)

        # Start up each of the shards
        for shard in self.shards:
            shard.setup()

    def await_ready(self):
        # Wait for the config server
        if self.configsvr is not None:
            self.configsvr.await_ready()

        # Wait for each of the shards
        for shard in self.shards:
            shard.await_ready()

        if self.mongos is None:
            self.mongos = self._new_mongos()

        # Start up the mongos
        self.mongos.setup()

        # Wait for the mongos
        self.mongos.await_ready()

        client = self.mongo_client()
        self._auth_to_db(client)

        # Inform mongos about each of the shards
        for shard in self.shards:
            self._add_shard(client, shard)

        # Enable sharding on each of the specified databases
        for db_name in self.enable_sharding:
            self.logger.info("Enabling sharding for '%s' database...", db_name)
            client.admin.command({"enablesharding": db_name})

        # Ensure that the sessions collection gets auto-sharded by the config server
        if self.configsvr is not None:
            primary = self.configsvr.get_primary().mongo_client()
            primary.admin.command({ "refreshLogicalSessionCacheNow" : 1 })


    def _auth_to_db(self, client):
        """Authenticate client for the 'authenticationDatabase'."""
        if self.auth_options is not None:
            auth_db = client[self.auth_options["authenticationDatabase"]]
            auth_db.authenticate(self.auth_options["username"],
                                 password=self.auth_options["password"],
                                 mechanism=self.auth_options["authenticationMechanism"])

    def _stop_balancer(self, timeout_ms=60000):
        """Stop the balancer."""
        client = self.mongo_client()
        self._auth_to_db(client)
        client.admin.command({"balancerStop": 1}, maxTimeMS=timeout_ms)

    def _do_teardown(self):
        """
        Shuts down the sharded cluster.
        """
        running_at_start = self.is_running()
        success = True  # Still a success even if nothing is running.

        if not running_at_start:
            self.logger.info(
                "Sharded cluster was expected to be running in _do_teardown(), but wasn't.")

        self._stop_balancer()

        if self.configsvr is not None:
            if running_at_start:
                self.logger.info("Stopping config server...")

            success = self.configsvr.teardown() and success

            if running_at_start:
                self.logger.info("Successfully terminated the config server.")

        if self.mongos is not None:
            if running_at_start:
                self.logger.info("Stopping mongos...")

            success = self.mongos.teardown() and success

            if running_at_start:
                self.logger.info("Successfully terminated the mongos.")

        if running_at_start:
            self.logger.info("Stopping shards...")
        for shard in self.shards:
            success = shard.teardown() and success
        if running_at_start:
            self.logger.info("Successfully terminated all shards.")

        return success

    def is_running(self):
        """
        Returns true if the config server, all shards, and the mongos
        are all still operating, and false otherwise.
        """
        return (self.configsvr is not None and self.configsvr.is_running() and
                all(shard.is_running() for shard in self.shards) and
                self.mongos is not None and self.mongos.is_running())

    def get_internal_connection_string(self):
        if self.mongos is None:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")

        return self.mongos.get_internal_connection_string()

    def get_driver_connection_url(self):
        return "mongodb://" + self.get_internal_connection_string()

    def _new_configsvr(self):
        """
        Returns a replicaset.ReplicaSetFixture configured to be used as
        the config server of a sharded cluster.
        """

        mongod_logger = self.logger.new_fixture_node_logger("configsvr")

        configsvr_options = self.configsvr_options.copy()

        auth_options = configsvr_options.pop("auth_options", self.auth_options)
        mongod_executable = configsvr_options.pop("mongod_executable", self.mongod_executable)
        preserve_dbpath = configsvr_options.pop("preserve_dbpath", self.preserve_dbpath)
        num_nodes = configsvr_options.pop("num_nodes", 1)

        replset_config_options = configsvr_options.pop("replset_config_options", {})
        replset_config_options["configsvr"] = True

        mongod_options = self.mongod_options.copy()
        mongod_options.update(configsvr_options.pop("mongod_options", {}))
        mongod_options["configsvr"] = ""
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "config")
        mongod_options["replSet"] = ShardedClusterFixture._CONFIGSVR_REPLSET_NAME
        mongod_options["storageEngine"] = "wiredTiger"

        return replicaset.ReplicaSetFixture(mongod_logger,
                                            self.job_num,
                                            mongod_executable=mongod_executable,
                                            mongod_options=mongod_options,
                                            preserve_dbpath=preserve_dbpath,
                                            num_nodes=num_nodes,
                                            auth_options=auth_options,
                                            replset_config_options=replset_config_options,
                                            **configsvr_options)

    def _new_rs_shard(self, index, num_rs_nodes_per_shard):
        """
        Returns a replicaset.ReplicaSetFixture configured to be used as a
        shard in a sharded cluster.
        """

        mongod_logger = self.logger.new_fixture_node_logger("shard{}".format(index))

        shard_options = self.shard_options.copy()

        auth_options = shard_options.pop("auth_options", self.auth_options)
        mongod_executable = shard_options.pop("mongod_executable", self.mongod_executable)
        preserve_dbpath = shard_options.pop("preserve_dbpath", self.preserve_dbpath)

        replset_config_options = shard_options.pop("replset_config_options", {})
        replset_config_options["configsvr"] = False

        mongod_options = self.mongod_options.copy()
        mongod_options.update(shard_options.pop("mongod_options", {}))
        mongod_options["shardsvr"] = ""
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "shard{}".format(index))
        mongod_options["replSet"] = ShardedClusterFixture._SHARD_REPLSET_NAME_PREFIX + str(index)

        return replicaset.ReplicaSetFixture(mongod_logger,
                                            self.job_num,
                                            mongod_executable=mongod_executable,
                                            mongod_options=mongod_options,
                                            preserve_dbpath=preserve_dbpath,
                                            num_nodes=num_rs_nodes_per_shard,
                                            auth_options=auth_options,
                                            replset_config_options=replset_config_options,
                                            **shard_options)

    def _new_standalone_shard(self, index):
        """
        Returns a standalone.MongoDFixture configured to be used as a
        shard in a sharded cluster.
        """

        mongod_logger = self.logger.new_fixture_node_logger("shard{}".format(index))

        shard_options = self.shard_options.copy()

        mongod_executable = shard_options.pop("mongod_executable", self.mongod_executable)
        preserve_dbpath = shard_options.pop("preserve_dbpath", self.preserve_dbpath)

        mongod_options = self.mongod_options.copy()
        mongod_options.update(shard_options.pop("mongod_options", {}))
        mongod_options["shardsvr"] = ""
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "shard{}".format(index))

        return standalone.MongoDFixture(mongod_logger,
                                        self.job_num,
                                        mongod_executable=mongod_executable,
                                        mongod_options=mongod_options,
                                        preserve_dbpath=preserve_dbpath,
                                        **shard_options)

    def _new_mongos(self):
        """
        Returns a _MongoSFixture configured to be used as the mongos for
        a sharded cluster.
        """

        mongos_logger = self.logger.new_fixture_node_logger("mongos")

        mongos_options = self.mongos_options.copy()

        if self.separate_configsvr:
            mongos_options["configdb"] = self.configsvr.get_internal_connection_string()
        else:
            mongos_options["configdb"] = "localhost:{}".format(self.shards[0].port)

        return _MongoSFixture(mongos_logger,
                              self.job_num,
                              mongos_executable=self.mongos_executable,
                              mongos_options=mongos_options)

    def _add_shard(self, client, shard):
        """
        Add the specified program as a shard by executing the addShard
        command.

        See https://docs.mongodb.org/manual/reference/command/addShard
        for more details.
        """

        connection_string = shard.get_internal_connection_string()
        self.logger.info("Adding %s as a shard...", connection_string)
        client.admin.command({"addShard": connection_string})


class _MongoSFixture(interface.Fixture):
    """
    Fixture which provides JSTests with a mongos to connect to.
    """

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self,
                 logger,
                 job_num,
                 mongos_executable=None,
                 mongos_options=None):

        interface.Fixture.__init__(self, logger, job_num)

        # Command line options override the YAML configuration.
        self.mongos_executable = utils.default_if_none(config.MONGOS_EXECUTABLE, mongos_executable)

        self.mongos_options = utils.default_if_none(mongos_options, {}).copy()

        self.mongos = None
        self.port = None

    def setup(self):
        if "port" not in self.mongos_options:
            self.mongos_options["port"] = core.network.PortAllocator.next_fixture_port(self.job_num)
        self.port = self.mongos_options["port"]

        mongos = core.programs.mongos_program(self.logger,
                                              executable=self.mongos_executable,
                                              **self.mongos_options)
        try:
            self.logger.info("Starting mongos on port %d...\n%s", self.port, mongos.as_command())
            mongos.start()
            self.logger.info("mongos started on port %d with pid %d.", self.port, mongos.pid)
        except:
            self.logger.exception("Failed to start mongos on port %d.", self.port)
            raise

        self.mongos = mongos

    def await_ready(self):
        deadline = time.time() + standalone.MongoDFixture.AWAIT_READY_TIMEOUT_SECS

        # Wait until the mongos is accepting connections. The retry logic is necessary to support
        # versions of PyMongo <3.0 that immediately raise a ConnectionFailure if a connection cannot
        # be established.
        while True:
            # Check whether the mongos exited for some reason.
            exit_code = self.mongos.poll()
            if exit_code is not None:
                raise errors.ServerFailure("Could not connect to mongos on port {}, process ended"
                                           " unexpectedly with code {}.".format(self.port,
                                                                                exit_code))

            try:
                # Use a shorter connection timeout to more closely satisfy the requested deadline.
                client = self.mongo_client(timeout_millis=500)
                client.admin.command("ping")
                break
            except pymongo.errors.ConnectionFailure:
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    raise errors.ServerFailure(
                        "Failed to connect to mongos on port {} after {} seconds".format(
                            self.port, standalone.MongoDFixture.AWAIT_READY_TIMEOUT_SECS))

                self.logger.info("Waiting to connect to mongos on port %d.", self.port)
                time.sleep(0.1)  # Wait a little bit before trying again.

        self.logger.info("Successfully contacted the mongos on port %d.", self.port)

    def _do_teardown(self):
        running_at_start = self.is_running()
        success = True  # Still a success even if nothing is running.

        if not running_at_start and self.mongos is not None:
            self.logger.info(
                "mongos on port %d was expected to be running in _do_teardown(), but wasn't. "
                "Exited with code %d.",
                self.port, self.mongos.poll())

        if self.mongos is not None:
            if running_at_start:
                self.logger.info("Stopping mongos on port %d with pid %d...",
                                 self.port,
                                 self.mongos.pid)
                self.mongos.stop()

            exit_code = self.mongos.wait()
            success = exit_code == 0

            if running_at_start:
                self.logger.info("Successfully terminated the mongos on port %d, exited with code"
                                 " %d",
                                 self.port,
                                 exit_code)

        return success

    def is_running(self):
        return self.mongos is not None and self.mongos.poll() is None

    def get_internal_connection_string(self):
        if self.mongos is None:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")

        return "localhost:%d" % self.port

    def get_driver_connection_url(self):
        return "mongodb://" + self.get_internal_connection_string()
