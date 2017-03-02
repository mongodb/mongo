"""
Sharded cluster fixture for executing JSTests against.
"""

from __future__ import absolute_import

import copy
import os.path
import socket
import time

import pybongo

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
                 bongos_executable=None,
                 bongos_options=None,
                 bongod_executable=None,
                 bongod_options=None,
                 dbpath_prefix=None,
                 preserve_dbpath=False,
                 num_shards=1,
                 separate_configsvr=True,
                 enable_sharding=None,
                 auth_options=None):
        """
        Initializes ShardedClusterFixture with the different options to
        the bongod and bongos processes.
        """

        interface.Fixture.__init__(self, logger, job_num)

        if "dbpath" in bongod_options:
            raise ValueError("Cannot specify bongod_options.dbpath")

        self.bongos_executable = bongos_executable
        self.bongos_options = utils.default_if_none(bongos_options, {})
        self.bongod_executable = bongod_executable
        self.bongod_options = utils.default_if_none(bongod_options, {})
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
        self.bongos = None
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

        if self.bongos is None:
            self.bongos = self._new_bongos()

        # Start up the bongos
        self.bongos.setup()

        # Wait for the bongos
        self.bongos.await_ready()
        self.port = self.bongos.port

        client = utils.new_bongo_client(port=self.port)
        if self.auth_options is not None:
            auth_db = client[self.auth_options["authenticationDatabase"]]
            auth_db.authenticate(self.auth_options["username"],
                                 password=self.auth_options["password"],
                                 mechanism=self.auth_options["authenticationMechanism"])

        # Inform bongos about each of the shards
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

        if self.bongos is not None:
            if running_at_start:
                self.logger.info("Stopping bongos...")

            success = self.bongos.teardown() and success

            if running_at_start:
                self.logger.info("Successfully terminated the bongos.")

        if running_at_start:
            self.logger.info("Stopping shards...")
        for shard in self.shards:
            success = shard.teardown() and success
        if running_at_start:
            self.logger.info("Successfully terminated all shards.")

        return success

    def is_running(self):
        """
        Returns true if the config server, all shards, and the bongos
        are all still operating, and false otherwise.
        """
        return (self.configsvr is not None and self.configsvr.is_running() and
                all(shard.is_running() for shard in self.shards) and
                self.bongos is not None and self.bongos.is_running())

    def _new_configsvr(self):
        """
        Returns a replicaset.ReplicaSetFixture configured to be used as
        the config server of a sharded cluster.
        """

        logger_name = "%s:configsvr" % (self.logger.name)
        bongod_logger = logging.loggers.new_logger(logger_name, parent=self.logger)

        bongod_options = copy.deepcopy(self.bongod_options)
        bongod_options["configsvr"] = ""
        bongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "config")
        bongod_options["replSet"] = ShardedClusterFixture._CONFIGSVR_REPLSET_NAME
        bongod_options["storageEngine"] = "wiredTiger"

        return replicaset.ReplicaSetFixture(bongod_logger,
                                            self.job_num,
                                            bongod_executable=self.bongod_executable,
                                            bongod_options=bongod_options,
                                            preserve_dbpath=self.preserve_dbpath,
                                            num_nodes=3,
                                            auth_options=self.auth_options,
                                            replset_config_options={"configsvr": True})

    def _new_shard(self, index):
        """
        Returns a standalone.BongoDFixture configured to be used as a
        shard in a sharded cluster.
        """

        logger_name = "%s:shard%d" % (self.logger.name, index)
        bongod_logger = logging.loggers.new_logger(logger_name, parent=self.logger)

        bongod_options = copy.deepcopy(self.bongod_options)
        bongod_options["shardsvr"] = ""
        bongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "shard%d" % (index))

        return standalone.BongoDFixture(bongod_logger,
                                        self.job_num,
                                        bongod_executable=self.bongod_executable,
                                        bongod_options=bongod_options,
                                        preserve_dbpath=self.preserve_dbpath)

    def _new_bongos(self):
        """
        Returns a _BongoSFixture configured to be used as the bongos for
        a sharded cluster.
        """

        logger_name = "%s:bongos" % (self.logger.name)
        bongos_logger = logging.loggers.new_logger(logger_name, parent=self.logger)

        bongos_options = copy.deepcopy(self.bongos_options)
        configdb_hostname = socket.gethostname()

        if self.separate_configsvr:
            configdb_replset = ShardedClusterFixture._CONFIGSVR_REPLSET_NAME
            configdb_port = self.configsvr.port
            bongos_options["configdb"] = "%s/%s:%d" % (configdb_replset,
                                                       configdb_hostname,
                                                       configdb_port)
        else:
            bongos_options["configdb"] = "%s:%d" % (configdb_hostname, self.shards[0].port)

        return _BongoSFixture(bongos_logger,
                              self.job_num,
                              bongos_executable=self.bongos_executable,
                              bongos_options=bongos_options)

    def _add_shard(self, client, shard):
        """
        Add the specified program as a shard by executing the addShard
        command.

        See https://docs.bongodb.org/manual/reference/command/addShard
        for more details.
        """

        hostname = socket.gethostname()
        self.logger.info("Adding %s:%d as a shard..." % (hostname, shard.port))
        client.admin.command({"addShard": "%s:%d" % (hostname, shard.port)})


class _BongoSFixture(interface.Fixture):
    """
    Fixture which provides JSTests with a bongos to connect to.
    """

    def __init__(self,
                 logger,
                 job_num,
                 bongos_executable=None,
                 bongos_options=None):

        interface.Fixture.__init__(self, logger, job_num)

        # Command line options override the YAML configuration.
        self.bongos_executable = utils.default_if_none(config.BONGOS_EXECUTABLE, bongos_executable)

        self.bongos_options = utils.default_if_none(bongos_options, {}).copy()

        self.bongos = None

    def setup(self):
        if "port" not in self.bongos_options:
            self.bongos_options["port"] = core.network.PortAllocator.next_fixture_port(self.job_num)
        self.port = self.bongos_options["port"]

        bongos = core.programs.bongos_program(self.logger,
                                              executable=self.bongos_executable,
                                              **self.bongos_options)
        try:
            self.logger.info("Starting bongos on port %d...\n%s", self.port, bongos.as_command())
            bongos.start()
            self.logger.info("bongos started on port %d with pid %d.", self.port, bongos.pid)
        except:
            self.logger.exception("Failed to start bongos on port %d.", self.port)
            raise

        self.bongos = bongos

    def await_ready(self):
        deadline = time.time() + standalone.BongoDFixture.AWAIT_READY_TIMEOUT_SECS

        # Wait until the bongos is accepting connections. The retry logic is necessary to support
        # versions of PyBongo <3.0 that immediately raise a ConnectionFailure if a connection cannot
        # be established.
        while True:
            # Check whether the bongos exited for some reason.
            exit_code = self.bongos.poll()
            if exit_code is not None:
                raise errors.ServerFailure("Could not connect to bongos on port %d, process ended"
                                           " unexpectedly with code %d." % (self.port, exit_code))

            try:
                # Use a shorter connection timeout to more closely satisfy the requested deadline.
                client = utils.new_bongo_client(self.port, timeout_millis=500)
                client.admin.command("ping")
                break
            except pybongo.errors.ConnectionFailure:
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    raise errors.ServerFailure(
                        "Failed to connect to bongos on port %d after %d seconds"
                        % (self.port, standalone.BongoDFixture.AWAIT_READY_TIMEOUT_SECS))

                self.logger.info("Waiting to connect to bongos on port %d.", self.port)
                time.sleep(0.1)  # Wait a little bit before trying again.

        self.logger.info("Successfully contacted the bongos on port %d.", self.port)

    def teardown(self):
        running_at_start = self.is_running()
        success = True  # Still a success even if nothing is running.

        if not running_at_start and self.port is not None:
            self.logger.info("bongos on port %d was expected to be running in teardown(), but"
                             " wasn't." % (self.port))

        if self.bongos is not None:
            if running_at_start:
                self.logger.info("Stopping bongos on port %d with pid %d...",
                                 self.port,
                                 self.bongos.pid)
                self.bongos.stop()

            exit_code = self.bongos.wait()
            success = exit_code == 0

            if running_at_start:
                self.logger.info("Successfully terminated the bongos on port %d, exited with code"
                                 " %d",
                                 self.port,
                                 exit_code)

        return success

    def is_running(self):
        return self.bongos is not None and self.bongos.poll() is None
