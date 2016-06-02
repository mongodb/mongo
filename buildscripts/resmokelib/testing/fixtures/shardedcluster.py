"""
Sharded cluster fixture for executing JSTests against.
"""

from __future__ import absolute_import

import copy
import os.path
import socket
import time

import pymongo

from . import interface
from . import standalone
from . import replicaset
from ... import config
from ... import core
from ... import errors
from ... import logging
from ... import utils


class ShardedClusterFixture(interface.Fixture):
    """
    Fixture which provides JSTests with a sharded cluster to run
    against.
    """

    _CONFIGSVR_REPLSET_NAME = "config-rs"

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
                 separate_configsvr=True,
                 enable_sharding=None,
                 auth_options=None):
        """
        Initializes ShardedClusterFixture with the different options to
        the mongod and mongos processes.
        """

        interface.Fixture.__init__(self, logger, job_num)

        if "dbpath" in mongod_options:
            raise ValueError("Cannot specify mongod_options.dbpath")

        self.mongos_executable = mongos_executable
        self.mongos_options = utils.default_if_none(mongos_options, {})
        self.mongod_executable = mongod_executable
        self.mongod_options = utils.default_if_none(mongod_options, {})
        self.preserve_dbpath = preserve_dbpath
        self.num_shards = num_shards
        self.separate_configsvr = separate_configsvr
        self.enable_sharding = utils.default_if_none(enable_sharding, [])
        self.auth_options = auth_options

        # Command line options override the YAML configuration.
        dbpath_prefix = utils.default_if_none(config.DBPATH_PREFIX, dbpath_prefix)
        dbpath_prefix = utils.default_if_none(dbpath_prefix, config.DEFAULT_DBPATH_PREFIX)
        self._dbpath_prefix = os.path.join(dbpath_prefix,
                                           "job%d" % (self.job_num),
                                           config.FIXTURE_SUBDIR)

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
                shard = self._new_shard(i)
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
        self.port = self.mongos.port

        client = utils.new_mongo_client(port=self.port)
        if self.auth_options is not None:
            auth_db = client[self.auth_options["authenticationDatabase"]]
            auth_db.authenticate(self.auth_options["username"],
                                 password=self.auth_options["password"],
                                 mechanism=self.auth_options["authenticationMechanism"])

        # Inform mongos about each of the shards
        for shard in self.shards:
            self._add_shard(client, shard)

        # Enable sharding on each of the specified databases
        for db_name in self.enable_sharding:
            self.logger.info("Enabling sharding for '%s' database...", db_name)
            client.admin.command({"enablesharding": db_name})

    def teardown(self):
        """
        Shuts down the sharded cluster.
        """
        running_at_start = self.is_running()
        success = True  # Still a success even if nothing is running.

        if not running_at_start:
            self.logger.info("Sharded cluster was expected to be running in teardown(), but"
                             " wasn't.")

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

    def _new_configsvr(self):
        """
        Returns a replicaset.ReplicaSetFixture configured to be used as
        the config server of a sharded cluster.
        """

        logger_name = "%s:configsvr" % (self.logger.name)
        mongod_logger = logging.loggers.new_logger(logger_name, parent=self.logger)

        mongod_options = copy.deepcopy(self.mongod_options)
        mongod_options["configsvr"] = ""
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "config")
        mongod_options["replSet"] = ShardedClusterFixture._CONFIGSVR_REPLSET_NAME
        mongod_options["storageEngine"] = "wiredTiger"

        return replicaset.ReplicaSetFixture(mongod_logger,
                                            self.job_num,
                                            mongod_executable=self.mongod_executable,
                                            mongod_options=mongod_options,
                                            preserve_dbpath=self.preserve_dbpath,
                                            num_nodes=3,
                                            auth_options=self.auth_options,
                                            replset_config_options={"configsvr": True})

    def _new_shard(self, index):
        """
        Returns a standalone.MongoDFixture configured to be used as a
        shard in a sharded cluster.
        """

        logger_name = "%s:shard%d" % (self.logger.name, index)
        mongod_logger = logging.loggers.new_logger(logger_name, parent=self.logger)

        mongod_options = copy.deepcopy(self.mongod_options)
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "shard%d" % (index))

        return standalone.MongoDFixture(mongod_logger,
                                        self.job_num,
                                        mongod_executable=self.mongod_executable,
                                        mongod_options=mongod_options,
                                        preserve_dbpath=self.preserve_dbpath)

    def _new_mongos(self):
        """
        Returns a _MongoSFixture configured to be used as the mongos for
        a sharded cluster.
        """

        logger_name = "%s:mongos" % (self.logger.name)
        mongos_logger = logging.loggers.new_logger(logger_name, parent=self.logger)

        mongos_options = copy.deepcopy(self.mongos_options)
        configdb_hostname = socket.gethostname()

        if self.separate_configsvr:
            configdb_replset = ShardedClusterFixture._CONFIGSVR_REPLSET_NAME
            configdb_port = self.configsvr.port
            mongos_options["configdb"] = "%s/%s:%d" % (configdb_replset,
                                                       configdb_hostname,
                                                       configdb_port)
        else:
            mongos_options["configdb"] = "%s:%d" % (configdb_hostname, self.shards[0].port)

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

        hostname = socket.gethostname()
        self.logger.info("Adding %s:%d as a shard..." % (hostname, shard.port))
        client.admin.command({"addShard": "%s:%d" % (hostname, shard.port)})


class _MongoSFixture(interface.Fixture):
    """
    Fixture which provides JSTests with a mongos to connect to.
    """

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
                raise errors.ServerFailure("Could not connect to mongos on port %d, process ended"
                                           " unexpectedly with code %d." % (self.port, exit_code))

            try:
                # Use a shorter connection timeout to more closely satisfy the requested deadline.
                client = utils.new_mongo_client(self.port, timeout_millis=500)
                client.admin.command("ping")
                break
            except pymongo.errors.ConnectionFailure:
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    raise errors.ServerFailure(
                        "Failed to connect to mongos on port %d after %d seconds"
                        % (self.port, standalone.MongoDFixture.AWAIT_READY_TIMEOUT_SECS))

                self.logger.info("Waiting to connect to mongos on port %d.", self.port)
                time.sleep(0.1)  # Wait a little bit before trying again.

        self.logger.info("Successfully contacted the mongos on port %d.", self.port)

    def teardown(self):
        running_at_start = self.is_running()
        success = True  # Still a success even if nothing is running.

        if not running_at_start and self.port is not None:
            self.logger.info("mongos on port %d was expected to be running in teardown(), but"
                             " wasn't." % (self.port))

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
