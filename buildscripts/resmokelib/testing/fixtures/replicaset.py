"""Replica set fixture for executing JSTests against."""

import os.path
import time

import bson.errors
import pymongo
import pymongo.errors
import pymongo.write_concern

from . import interface
from . import replicaset_utils
from . import standalone
from ... import config
from ... import errors
from ... import utils


class ReplicaSetFixture(interface.ReplFixture):  # pylint: disable=too-many-instance-attributes
    """Fixture which provides JSTests with a replica set to run against."""

    def __init__(  # pylint: disable=too-many-arguments, too-many-locals
            self, logger, job_num, mongod_executable=None, mongod_options=None, dbpath_prefix=None,
            preserve_dbpath=False, num_nodes=2, start_initial_sync_node=False,
            write_concern_majority_journal_default=None, auth_options=None,
            replset_config_options=None, voting_secondaries=None, all_nodes_electable=False,
            use_replica_set_connection_string=None, linear_chain=False):
        """Initialize ReplicaSetFixture."""

        interface.ReplFixture.__init__(self, logger, job_num, dbpath_prefix=dbpath_prefix)

        self.mongod_executable = mongod_executable
        self.mongod_options = utils.default_if_none(mongod_options, {})
        self.preserve_dbpath = preserve_dbpath
        self.num_nodes = num_nodes
        self.start_initial_sync_node = start_initial_sync_node
        self.write_concern_majority_journal_default = write_concern_majority_journal_default
        self.auth_options = auth_options
        self.replset_config_options = utils.default_if_none(replset_config_options, {})
        self.voting_secondaries = voting_secondaries
        self.all_nodes_electable = all_nodes_electable
        self.use_replica_set_connection_string = use_replica_set_connection_string
        self.linear_chain = linear_chain

        # If voting_secondaries has not been set, set a default. By default, secondaries have zero
        # votes unless they are also nodes capable of being elected primary.
        if self.voting_secondaries is None:
            self.voting_secondaries = self.all_nodes_electable

        # By default, we only use a replica set connection string if all nodes are capable of being
        # elected primary.
        if self.use_replica_set_connection_string is None:
            self.use_replica_set_connection_string = self.all_nodes_electable

        # Set the default oplogSize to 511MB.
        self.mongod_options.setdefault("oplogSize", 511)

        # The dbpath in mongod_options is used as the dbpath prefix for replica set members and
        # takes precedence over other settings. The ShardedClusterFixture uses this parameter to
        # create replica sets and assign their dbpath structure explicitly.
        if "dbpath" in self.mongod_options:
            self._dbpath_prefix = self.mongod_options.pop("dbpath")
        else:
            self._dbpath_prefix = os.path.join(self._dbpath_prefix, config.FIXTURE_SUBDIR)

        self.nodes = []
        self.replset_name = None
        self.initial_sync_node = None
        self.initial_sync_node_idx = -1

    def setup(self):  # pylint: disable=too-many-branches,too-many-statements
        """Set up the replica set."""
        self.replset_name = self.mongod_options.get("replSet", "rs")

        if not self.nodes:
            for i in range(self.num_nodes):
                node = self._new_mongod(i, self.replset_name)
                self.nodes.append(node)

        for i in range(self.num_nodes):
            if self.linear_chain and i > 0:
                self.nodes[i].mongod_options["set_parameters"][
                    "failpoint.forceSyncSourceCandidate"] = {
                        "mode": "alwaysOn",
                        "data": {"hostAndPort": self.nodes[i - 1].get_internal_connection_string()}
                    }
            self.nodes[i].setup()

        if self.start_initial_sync_node:
            if not self.initial_sync_node:
                self.initial_sync_node_idx = len(self.nodes)
                self.initial_sync_node = self._new_mongod(self.initial_sync_node_idx,
                                                          self.replset_name)
            self.initial_sync_node.setup()
            self.initial_sync_node.await_ready()

        # We need only to wait to connect to the first node of the replica set because we first
        # initiate it as a single node replica set.
        self.nodes[0].await_ready()

        # Initiate the replica set.
        members = []
        for (i, node) in enumerate(self.nodes):
            member_info = {"_id": i, "host": node.get_internal_connection_string()}
            if i > 0:
                if not self.all_nodes_electable:
                    member_info["priority"] = 0
                if i >= 7 or not self.voting_secondaries:
                    # Only 7 nodes in a replica set can vote, so the other members must still be
                    # non-voting when this fixture is configured to have voting secondaries.
                    member_info["votes"] = 0
            members.append(member_info)
        if self.initial_sync_node:
            members.append({
                "_id": self.initial_sync_node_idx,
                "host": self.initial_sync_node.get_internal_connection_string(), "priority": 0,
                "hidden": 1, "votes": 0
            })

        repl_config = {"_id": self.replset_name, "protocolVersion": 1}
        client = self.nodes[0].mongo_client()

        self.auth(client, self.auth_options)

        if client.local.system.replset.count():
            # Skip initializing the replset if there is an existing configuration.
            return

        if self.write_concern_majority_journal_default is not None:
            repl_config[
                "writeConcernMajorityJournalDefault"] = self.write_concern_majority_journal_default
        else:
            server_status = client.admin.command({"serverStatus": 1})
            cmd_line_opts = client.admin.command({"getCmdLineOpts": 1})
            if not (server_status["storageEngine"]["persistent"] and cmd_line_opts["parsed"].get(
                    "storage", {}).get("journal", {}).get("enabled", True)):
                repl_config["writeConcernMajorityJournalDefault"] = False

        if self.replset_config_options.get("configsvr", False):
            repl_config["configsvr"] = True
        if self.replset_config_options.get("settings"):
            replset_settings = self.replset_config_options["settings"]
            repl_config["settings"] = replset_settings

        # If secondaries vote and no election timeout was specified, then we increase the election
        # timeout to 24 hours to prevent spurious elections.
        if self.voting_secondaries:
            repl_config.setdefault("settings", {})
            if "electionTimeoutMillis" not in repl_config["settings"]:
                repl_config["settings"]["electionTimeoutMillis"] = 24 * 60 * 60 * 1000

        # Start up a single node replica set then reconfigure to the correct size (if the config
        # contains more than 1 node), so the primary is elected more quickly.
        repl_config["members"] = [members[0]]
        self.logger.info("Issuing replSetInitiate command: %s", repl_config)
        self._configure_repl_set(client, {"replSetInitiate": repl_config})
        self._await_primary()

        if self.nodes[1:]:
            # Wait to connect to each of the secondaries before running the replSetReconfig
            # command.
            for node in self.nodes[1:]:
                node.await_ready()
            repl_config["version"] = 2
            repl_config["members"] = members
            self.logger.info("Issuing replSetReconfig command: %s", repl_config)
            self._configure_repl_set(client, {"replSetReconfig": repl_config})
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
                # Retry on NodeNotFound errors from the "replSetInitiate" command.
                if err.code != ReplicaSetFixture._NODE_NOT_FOUND:
                    msg = ("Operation failure while configuring the "
                           "replica set fixture: {}").format(err)
                    self.logger.error(msg)
                    raise errors.ServerFailure(msg)

                msg = "replSetInitiate failed attempt {0} of {1} with error: {2}".format(
                    attempt, num_initiate_attempts, err)
                self.logger.error(msg)
                if attempt == num_initiate_attempts:
                    msg = "Exceeded number of retries while configuring the replica set fixture"
                    self.logger.error(msg + ".")
                    raise errors.ServerFailure(msg)
                time.sleep(5)  # Wait a little bit before trying again.

    def await_last_op_committed(self, timeout_secs=None):
        """Wait for the last majority committed op to be visible."""
        primary_client = self.get_primary().mongo_client()
        self.auth(primary_client, self.auth_options)

        primary_optime = replicaset_utils.get_last_optime(primary_client)
        up_to_date_nodes = set()

        def check_rcmaj_optime(client, node):
            """Return True if all nodes have caught up with the primary."""
            # TODO SERVER-40078: The server is reporting invalid
            # dates in its response to the replSetGetStatus
            # command
            try:
                res = client.admin.command({"replSetGetStatus": 1})
            except bson.errors.InvalidBSON:
                return False
            read_concern_majority_optime = res["optimes"]["readConcernMajorityOpTime"]

            if (read_concern_majority_optime["t"] == primary_optime["t"]
                    and read_concern_majority_optime["ts"] >= primary_optime["ts"]):
                up_to_date_nodes.add(node.port)

            return len(up_to_date_nodes) == len(self.nodes)

        self._await_cmd_all_nodes(check_rcmaj_optime, "waiting for last committed optime",
                                  timeout_secs)

    def await_ready(self):
        """Wait for replica set to be ready."""
        self._await_primary()
        self._await_secondaries()
        self._await_stable_recovery_timestamp()
        self._setup_sessions_collection()

    def _await_primary(self):
        # Wait for the primary to be elected.
        # Since this method is called at startup we expect the first node to be primary even when
        # self.all_nodes_electable is True.
        primary = self.nodes[0]
        client = primary.mongo_client()
        while True:
            self.logger.info("Waiting for primary on port %d to be elected.", primary.port)
            is_master = client.admin.command("isMaster")["ismaster"]
            if is_master:
                break
            time.sleep(0.1)  # Wait a little bit before trying again.
        self.logger.info("Primary on port %d successfully elected.", primary.port)

    def _await_secondaries(self):
        # Wait for the secondaries to become available.
        # Since this method is called at startup we expect the nodes 1 to n to be secondaries even
        # when self.all_nodes_electable is True.
        secondaries = self.nodes[1:]
        if self.initial_sync_node:
            secondaries.append(self.initial_sync_node)

        for secondary in secondaries:
            client = secondary.mongo_client(read_preference=pymongo.ReadPreference.SECONDARY)
            while True:
                self.logger.info("Waiting for secondary on port %d to become available.",
                                 secondary.port)
                is_secondary = client.admin.command("isMaster")["secondary"]
                if is_secondary:
                    break
                time.sleep(0.1)  # Wait a little bit before trying again.
            self.logger.info("Secondary on port %d is now available.", secondary.port)

    @staticmethod
    def auth(client, auth_options=None):
        """Auth a client connection."""
        if auth_options is not None:
            auth_db = client[auth_options["authenticationDatabase"]]
            auth_db.authenticate(auth_options["username"], password=auth_options["password"],
                                 mechanism=auth_options["authenticationMechanism"])

        return client

    def _await_stable_recovery_timestamp(self):
        """
        Awaits stable recovery timestamps on all nodes in the replica set.

        Performs some writes and then waits for all nodes in this replica set to establish a stable
        recovery timestamp. The writes are necessary to prompt storage engines to quickly establish
        stable recovery timestamps.

        A stable recovery timestamp ensures recoverable rollback is possible, as well as startup
        recovery without re-initial syncing in the case of durable storage engines. By waiting for
        all nodes to report having a stable recovery timestamp, we ensure a degree of stability in
        our tests to run as expected.
        """

        # Since this method is called at startup we expect the first node to be primary even when
        # self.all_nodes_electable is True.
        primary_client = self.nodes[0].mongo_client()
        self.auth(primary_client, self.auth_options)

        # All nodes must be in primary/secondary state prior to this point. Perform a majority
        # write to ensure there is a committed operation on the set. The commit point will
        # propagate to all members and trigger a stable checkpoint on all persisted storage engines
        # nodes.
        admin = primary_client.get_database(
            "admin", write_concern=pymongo.write_concern.WriteConcern(w="majority"))
        admin.command("appendOplogNote", data={"await_stable_recovery_timestamp": 1})

        for node in self.nodes:
            self.logger.info("Waiting for node on port %d to have a stable recovery timestamp.",
                             node.port)
            client = node.mongo_client(read_preference=pymongo.ReadPreference.SECONDARY)
            self.auth(client, self.auth_options)

            client_admin = client["admin"]

            while True:
                # TODO SERVER-40078: The server is reporting invalid
                # dates in its response to the replSetGetStatus
                # command
                try:
                    status = client_admin.command("replSetGetStatus")
                except bson.errors.InvalidBSON:
                    time.sleep(0.1)
                    continue

                # The `lastStableRecoveryTimestamp` field contains a stable timestamp guaranteed to
                # exist on storage engine recovery to a stable timestamp.
                last_stable_recovery_timestamp = status.get("lastStableRecoveryTimestamp", None)

                # A missing `lastStableRecoveryTimestamp` field indicates that the storage
                # engine does not support "recover to a stable timestamp".
                if not last_stable_recovery_timestamp:
                    break

                # A null `lastStableRecoveryTimestamp` indicates that the storage engine supports
                # "recover to a stable timestamp" but does not have a stable recovery timestamp yet.
                if last_stable_recovery_timestamp.time:
                    self.logger.info(
                        "Node on port %d now has a stable timestamp for recovery. Time: %s",
                        node.port, last_stable_recovery_timestamp)
                    break
                time.sleep(0.1)  # Wait a little bit before trying again.

    def _setup_sessions_collection(self):
        """Set up the sessions collection so that it will not attempt to set up during a test."""
        primary = self.nodes[0]
        primary.mongo_client().admin.command({"refreshLogicalSessionCacheNow": 1})

    def _do_teardown(self):
        self.logger.info("Stopping all members of the replica set...")

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.info("All members of the replica set were expected to be running, "
                             "but weren't.")

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        if self.initial_sync_node:
            teardown_handler.teardown(self.initial_sync_node, "initial sync node")

        # Terminate the secondaries first to reduce noise in the logs.
        for node in reversed(self.nodes):
            teardown_handler.teardown(node, "replica set member on port %d" % node.port)

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped all members of the replica set.")
        else:
            self.logger.error("Stopping the replica set fixture failed.")
            raise errors.ServerFailure(teardown_handler.get_error_message())

    def is_running(self):
        """Return True if all nodes in the replica set are running."""
        running = all(node.is_running() for node in self.nodes)

        if self.initial_sync_node:
            running = self.initial_sync_node.is_running() or running

        return running

    def get_primary(self, timeout_secs=30):  # pylint: disable=arguments-differ
        """Return the primary from a replica set."""
        if not self.all_nodes_electable:
            # The primary is always the first element of the 'nodes' list because all other members
            # of the replica set are configured with priority=0.
            return self.nodes[0]

        def is_primary(client, node):
            """Return if `node` is master."""
            is_master = client.admin.command("isMaster")["ismaster"]
            if is_master:
                self.logger.info("The node on port %d is primary of replica set '%s'", node.port,
                                 self.replset_name)
                return True
            return False

        return self._await_cmd_all_nodes(is_primary, "waiting for a primary", timeout_secs)

    def _await_cmd_all_nodes(self, fn, msg, timeout_secs=None):
        """Run `fn` on all nodes until it returns a truthy value.

        Return the node for which makes `fn` become truthy.

        Two arguments are passed to fn: the client for a node and
        the MongoDFixture corresponding to that node.
        """

        if timeout_secs is None:
            timeout_secs = self.AWAIT_REPL_TIMEOUT_MINS * 60
        start = time.time()
        clients = {}
        while True:
            for node in self.nodes:
                now = time.time()
                if (now - start) >= timeout_secs:
                    msg = "Timed out while {} for replica set '{}'.".format(msg, self.replset_name)
                    self.logger.error(msg)
                    raise errors.ServerFailure(msg)

                try:
                    if node.port not in clients:
                        clients[node.port] = self.auth(node.mongo_client(), self.auth_options)

                    if fn(clients[node.port], node):
                        return node

                except pymongo.errors.AutoReconnect:
                    # AutoReconnect exceptions may occur if the primary stepped down since PyMongo
                    # last contacted it. We'll just try contacting the node again in the next round
                    # of isMaster requests.
                    continue

    def get_secondaries(self):
        """Return a list of secondaries from the replica set."""
        primary = self.get_primary()
        return [node for node in self.nodes if node.port != primary.port]

    def get_initial_sync_node(self):
        """Return initila sync node from the replica set."""
        return self.initial_sync_node

    def _new_mongod(self, index, replset_name):
        """Return a standalone.MongoDFixture configured to be used as replica-set member."""

        mongod_logger = self._get_logger_for_mongod(index)
        mongod_options = self.mongod_options.copy()
        mongod_options["replSet"] = replset_name
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "node{}".format(index))
        mongod_options["set_parameters"] = mongod_options.get("set_parameters", {}).copy()

        return standalone.MongoDFixture(
            mongod_logger, self.job_num, mongod_executable=self.mongod_executable,
            mongod_options=mongod_options, preserve_dbpath=self.preserve_dbpath)

    def _get_logger_for_mongod(self, index):
        """Return a new logging.Logger instance.

        The instance is used as the primary, secondary, or initial sync member of a replica-set.
        """

        if index == self.initial_sync_node_idx:
            node_name = "initsync"
        elif self.all_nodes_electable:
            node_name = "node{}".format(index)
        elif index == 0:
            node_name = "primary"
        else:
            suffix = str(index - 1) if self.num_nodes > 2 else ""
            node_name = "secondary{}".format(suffix)

        return self.logger.new_fixture_node_logger(node_name)

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        if self.replset_name is None:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")

        conn_strs = [node.get_internal_connection_string() for node in self.nodes]
        if self.initial_sync_node:
            conn_strs.append(self.initial_sync_node.get_internal_connection_string())
        return self.replset_name + "/" + ",".join(conn_strs)

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        if self.replset_name is None:
            raise ValueError("Must call setup() before calling get_driver_connection_url()")

        if self.use_replica_set_connection_string:
            # We use a replica set connection string when all nodes are electable because we
            # anticipate the client will want to gracefully handle any failovers.
            conn_strs = [node.get_internal_connection_string() for node in self.nodes]
            if self.initial_sync_node:
                conn_strs.append(self.initial_sync_node.get_internal_connection_string())
            return "mongodb://" + ",".join(conn_strs) + "/?replicaSet=" + self.replset_name
        else:
            # We return a direct connection to the expected pimary when only the first node is
            # electable because we want the client to error out if a stepdown occurs.
            return self.nodes[0].get_driver_connection_url()
