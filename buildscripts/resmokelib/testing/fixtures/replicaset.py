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

    # Error response codes copied from mongo/base/error_codes.err.
    _ALREADY_INITIALIZED = 23
    _NODE_NOT_FOUND = 74

    def __init__(self,
                 logger,
                 job_num,
                 mongod_executable=None,
                 mongod_options=None,
                 dbpath_prefix=None,
                 preserve_dbpath=False,
                 num_nodes=2,
                 start_initial_sync_node=False,
                 write_concern_majority_journal_default=None,
                 auth_options=None,
                 replset_config_options=None,
                 voting_secondaries=False):

        interface.ReplFixture.__init__(self, logger, job_num)

        self.mongod_executable = mongod_executable
        self.mongod_options = utils.default_if_none(mongod_options, {})
        self.preserve_dbpath = preserve_dbpath
        self.num_nodes = num_nodes
        self.start_initial_sync_node = start_initial_sync_node
        self.write_concern_majority_journal_default = write_concern_majority_journal_default
        self.auth_options = auth_options
        self.replset_config_options = utils.default_if_none(replset_config_options, {})
        self.voting_secondaries = voting_secondaries

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
        self.replset_name = None
        self.initial_sync_node = None
        self.initial_sync_node_idx = -1

    def setup(self):
        self.replset_name = self.mongod_options.get("replSet", "rs")

        if not self.nodes:
            for i in xrange(self.num_nodes):
                node = self._new_mongod(i, self.replset_name)
                self.nodes.append(node)

        for node in self.nodes:
            node.setup()

        if self.start_initial_sync_node:
            if not self.initial_sync_node:
                self.initial_sync_node_idx = len(self.nodes)
                self.initial_sync_node = self._new_mongod(self.initial_sync_node_idx,
                                                          self.replset_name)
            self.initial_sync_node.setup()
            self.initial_sync_node.await_ready()

        self.port = self.get_primary().port

        # We need only to wait to connect to the first node of the replica set because we first
        # initiate it as a single node replica set.
        self.get_primary().await_ready()

        # Initiate the replica set.
        members = []
        for (i, node) in enumerate(self.nodes):
            member_info = {"_id": i, "host": node.get_connection_string()}
            if i > 0:
                member_info["priority"] = 0
                if i >= 7 or not self.voting_secondaries:
                    # Only 7 nodes in a replica set can vote, so the other members must still be
                    # non-voting when this fixture is configured to have voting secondaries.
                    member_info["votes"] = 0
            members.append(member_info)
        if self.initial_sync_node:
            members.append({"_id": self.initial_sync_node_idx,
                            "host": self.initial_sync_node.get_connection_string(),
                            "priority": 0,
                            "hidden": 1,
                            "votes": 0})

        config = {"_id": self.replset_name}
        client = utils.new_mongo_client(port=self.port)

        if self.auth_options is not None:
            auth_db = client[self.auth_options["authenticationDatabase"]]
            auth_db.authenticate(self.auth_options["username"],
                                 password=self.auth_options["password"],
                                 mechanism=self.auth_options["authenticationMechanism"])

        if client.local.system.replset.count():
            # Skip initializing the replset if there is an existing configuration.
            return

        if self.write_concern_majority_journal_default is not None:
            config["writeConcernMajorityJournalDefault"] = self.write_concern_majority_journal_default
        else:
            serverStatus = client.admin.command({"serverStatus": 1})
            cmdLineOpts = client.admin.command({"getCmdLineOpts": 1})
            if not (serverStatus["storageEngine"]["persistent"] and
                    cmdLineOpts["parsed"].get("storage", {}).get("journal", {}).get("enabled", True)):
                config["writeConcernMajorityJournalDefault"] = False

        if self.replset_config_options.get("configsvr", False):
            config["configsvr"] = True
        if self.replset_config_options.get("settings"):
            replset_settings = self.replset_config_options["settings"]
            config["settings"] = replset_settings

        # Start up a single node replica set then reconfigure to the correct size (if the config
        # contains more than 1 node), so the primary is elected more quickly.
        config["members"] = [members[0]]
        self.logger.info("Issuing replSetInitiate command: %s", config)
        self._configure_repl_set(client, {"replSetInitiate": config})
        self._await_primary()

        if self.get_secondaries():
            # Wait to connect to each of the secondaries before running the replSetReconfig
            # command.
            for node in self.get_secondaries():
                node.await_ready()
            config["version"] = 2
            config["members"] = members
            self.logger.info("Issuing replSetReconfig command: %s", config)
            self._configure_repl_set(client, {"replSetReconfig": config})
            self._await_secondaries()

    def _configure_repl_set(self, client, cmd_obj):
        # replSetInitiate and replSetReconfig commands can fail with a NodeNotFound error
        # if a heartbeat times out during the quorum check. We retry three times to reduce
        # the chance of failing this way.
        num_initiate_attempts = 3
        for attempt in range(1, num_initiate_attempts + 1):
            try:
                client.admin.command(cmd_obj)
                break
            except pymongo.errors.OperationFailure as err:
                # Ignore errors from the "replSetInitiate" command when the replica set has already
                # been initiated.
                if err.code == ReplicaSetFixture._ALREADY_INITIALIZED:
                    return

                # Retry on NodeNotFound errors from the "replSetInitiate" command.
                if err.code != ReplicaSetFixture._NODE_NOT_FOUND:
                    raise

                msg = "replSetInitiate failed attempt {0} of {1} with error: {2}".format(
                    attempt, num_initiate_attempts, err)
                self.logger.error(msg)
                if attempt == num_initiate_attempts:
                    raise
                time.sleep(5)  # Wait a little bit before trying again.

    def await_ready(self):
        self._await_primary()
        self._await_secondaries()

    def _await_primary(self):
        # Wait for the primary to be elected.
        client = utils.new_mongo_client(port=self.port)
        while True:
            self.logger.info("Waiting for primary on port %d to be elected.", self.port)
            is_master = client.admin.command("isMaster")["ismaster"]
            if is_master:
                break
            time.sleep(0.1)  # Wait a little bit before trying again.
        self.logger.info("Primary on port %d successfully elected.", self.port)

    def _await_secondaries(self):
        # Wait for the secondaries to become available.
        secondaries = self.get_secondaries()
        if self.initial_sync_node:
            secondaries.append(self.initial_sync_node)

        for secondary in secondaries:
            client = utils.new_mongo_client(port=secondary.port,
                                            read_preference=pymongo.ReadPreference.SECONDARY)
            while True:
                self.logger.info("Waiting for secondary on port %d to become available.",
                                 secondary.port)
                is_secondary = client.admin.command("isMaster")["secondary"]
                if is_secondary:
                    break
                time.sleep(0.1)  # Wait a little bit before trying again.
            self.logger.info("Secondary on port %d is now available.", secondary.port)

    def _do_teardown(self):
        running_at_start = self.is_running()
        success = True  # Still a success even if nothing is running.

        if not running_at_start:
            self.logger.info(
                "Replica set was expected to be running in _do_teardown(), but wasn't.")
        else:
            self.logger.info("Stopping all members of the replica set...")

        if self.initial_sync_node:
            success = self.initial_sync_node.teardown() and success

        # Terminate the secondaries first to reduce noise in the logs.
        for node in reversed(self.nodes):
            success = node.teardown() and success

        if running_at_start:
            self.logger.info("Successfully stopped all members of the replica set.")

        return success

    def is_running(self):
        running = all(node.is_running() for node in self.nodes)

        if self.initial_sync_node:
            running = self.initial_sync_node.is_running() or running

        return running

    def get_primary(self):
        # The primary is always the first element of the 'nodes' list because all other members of
        # the replica set are configured with priority=0.
        return self.nodes[0]

    def get_secondaries(self):
        return self.nodes[1:]

    def get_initial_sync_node(self):
        return self.initial_sync_node

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
        Returns a new logging.Logger instance for use as the primary, secondary, or initial
        sync member of a replica-set.
        """

        if index == 0:
            logger_name = "%s:primary" % (self.logger.name)
        elif index == self.initial_sync_node_idx:
            logger_name = "%s:initsync" % (self.logger.name)
        else:
            suffix = str(index - 1) if self.num_nodes > 2 else ""
            logger_name = "%s:secondary%s" % (self.logger.name, suffix)

        return logging.loggers.new_logger(logger_name, parent=self.logger)

    def get_connection_string(self):
        if self.replset_name is None:
            raise ValueError("Must call setup() before calling get_connection_string()")

        conn_strs = [node.get_connection_string() for node in self.nodes]
        if self.initial_sync_node:
            conn_strs.append(self.initial_sync_node.get_connection_string())
        return self.replset_name + "/" + ",".join(conn_strs)
