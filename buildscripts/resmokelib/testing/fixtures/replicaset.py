"""
Replica set fixture for executing JSTests against.
"""

from __future__ import absolute_import

import os.path
import time

import pymongo

from . import interface
from . import standalone
from ... import config
from ... import logging
from ... import utils


class ReplicaSetFixture(interface.ReplFixture):
    """
    Fixture which provides JSTests with a replica set to run against.
    """

    def __init__(self,
                 logger,
                 job_num,
                 mongod_executable=None,
                 mongod_options=None,
                 dbpath_prefix=None,
                 preserve_dbpath=False,
                 num_nodes=2,
                 auth_options=None):

        interface.ReplFixture.__init__(self, logger, job_num)

        self.mongod_executable = mongod_executable
        self.mongod_options = utils.default_if_none(mongod_options, {})
        self.preserve_dbpath = preserve_dbpath
        self.num_nodes = num_nodes
        self.auth_options = auth_options

        # The dbpath in mongod_options is used as the dbpath prefix for replica set members and
        # takes precedence over other settings. The ShardedClusterFixture uses this parameter to
        # create replica sets and assign their dbpath structure explicitly.
        if "dbpath" in self.mongod_options:
            self._dbpath_prefix = self.mongod_options.pop("dbpath")
        else:
            # Command line options override the YAML configuration.
            dbpath_prefix = utils.default_if_none(config.DBPATH_PREFIX, dbpath_prefix)
            dbpath_prefix = utils.default_if_none(dbpath_prefix, config.DEFAULT_DBPATH_PREFIX)
            self._dbpath_prefix = os.path.join(dbpath_prefix,
                                               "job%d" % (self.job_num),
                                               config.FIXTURE_SUBDIR)

        self.nodes = []

    def setup(self):
        replset_name = self.mongod_options.get("replSet", "rs")

        for i in xrange(self.num_nodes):
            node = self._new_mongod(i, replset_name)
            node.setup()
            self.nodes.append(node)

        self.port = self.get_primary().port

        # Call await_ready() on each of the nodes here because we want to start the election as
        # soon as possible.
        for node in self.nodes:
            node.await_ready()

        # Initiate the replica set.
        members = []
        for (i, node) in enumerate(self.nodes):
            conn_str = "localhost:%d" % (node.port)
            member_info = {"_id": i, "host": conn_str}
            if i > 0:
                member_info["priority"] = 0
            members.append(member_info)
        initiate_cmd_obj = {"replSetInitiate": {"_id": replset_name, "members": members}}

        client = utils.new_mongo_client(port=self.port)
        if self.auth_options is not None:
            auth_db = client[self.auth_options["authenticationDatabase"]]
            auth_db.authenticate(self.auth_options["username"],
                                 password=self.auth_options["password"],
                                 mechanism=self.auth_options["authenticationMechanism"])

        self.logger.info("Issuing replSetInitiate command...")
        client.admin.command(initiate_cmd_obj)

    def await_ready(self):
        # Wait for the primary to be elected.
        client = utils.new_mongo_client(port=self.port)
        while True:
            is_master = client.admin.command("isMaster")["ismaster"]
            if is_master:
                break
            self.logger.info("Waiting for primary on port %d to be elected.", self.port)
            time.sleep(1)  # Wait a little bit before trying again.

        # Wait for the secondaries to become available.
        for secondary in self.get_secondaries():
            client = utils.new_mongo_client(port=secondary.port,
                                            read_preference=pymongo.ReadPreference.SECONDARY)
            while True:
                is_secondary = client.admin.command("isMaster")["secondary"]
                if is_secondary:
                    break
                self.logger.info("Waiting for secondary on port %d to become available.",
                                 secondary.port)
                time.sleep(1)  # Wait a little bit before trying again.

    def teardown(self):
        running_at_start = self.is_running()
        success = True  # Still a success even if nothing is running.

        if not running_at_start:
            self.logger.info("Replica set was expected to be running in teardown(), but wasn't.")
        else:
            self.logger.info("Stopping all members of the replica set...")

        # Terminate the secondaries first to reduce noise in the logs.
        for node in reversed(self.nodes):
            success = node.teardown() and success

        if running_at_start:
            self.logger.info("Successfully stopped all members of the replica set.")

        return success

    def is_running(self):
        return all(node.is_running() for node in self.nodes)

    def get_primary(self):
        # The primary is always the first element of the 'nodes' list because all other members of
        # the replica set are configured with priority=0.
        return self.nodes[0]

    def get_secondaries(self):
        return self.nodes[1:]

    def await_repl(self):
        self.logger.info("Awaiting replication of insert (w=%d, wtimeout=%d min) to primary on port"
                         " %d", self.num_nodes, interface.ReplFixture.AWAIT_REPL_TIMEOUT_MINS,
                         self.port)

        # Keep retrying this until it times out waiting for replication.
        def insert_fn(remaining_secs):
            remaining_millis = int(round(remaining_secs * 1000))
            client = utils.new_mongo_client(port=self.port)
            client.resmoke.await_repl.insert({"awaiting": "repl"},
                                             w=self.num_nodes,
                                             wtimeout=remaining_millis)
        try:
            self.retry_until_wtimeout(insert_fn)
        except pymongo.errors.WTimeoutError:
            self.logger.info("Replication of write operation timed out.")
            raise

        self.logger.info("Replication of write operation completed.")

    def _new_mongod(self, index, replset_name):
        """
        Returns a standalone.MongoDFixture configured to be used as a
        replica-set member of 'replset_name'.
        """

        mongod_logger = self._get_logger_for_mongod(index)
        mongod_options = self.mongod_options.copy()
        mongod_options["replSet"] = replset_name
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "node%d" % (index))

        return standalone.MongoDFixture(mongod_logger,
                                        self.job_num,
                                        mongod_executable=self.mongod_executable,
                                        mongod_options=mongod_options,
                                        preserve_dbpath=self.preserve_dbpath)

    def _get_logger_for_mongod(self, index):
        """
        Returns a new logging.Logger instance for use as the primary or
        secondary of a replica-set.
        """

        if index == 0:
            logger_name = "%s:primary" % (self.logger.name)
        else:
            suffix = str(index - 1) if self.num_nodes > 2 else ""
            logger_name = "%s:secondary%s" % (self.logger.name, suffix)

        return logging.loggers.new_logger(logger_name, parent=self.logger)
