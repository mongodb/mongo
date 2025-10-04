"""Replica set fixture for executing JSTests against."""

import os.path
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from threading import Lock
from typing import Optional

import bson
import pymongo
import pymongo.errors
import pymongo.write_concern

from buildscripts.resmokelib.extensions import (
    delete_extension_configs,
    find_and_generate_extension_configs,
)
from buildscripts.resmokelib.testing.fixtures import interface


def compare_timestamp(timestamp1, timestamp2):
    """Compare the timestamp object ts part."""
    if timestamp1.time == timestamp2.time:
        if timestamp1.inc < timestamp2.inc:
            return -1
        elif timestamp1.inc > timestamp2.inc:
            return 1
        else:
            return 0
    elif timestamp1.time < timestamp2.time:
        return -1
    else:
        return 1


def compare_optime(optime1, optime2):
    """Compare timestamp object t part."""
    if optime1["t"] > optime2["t"]:
        return 1
    elif optime1["t"] < optime2["t"]:
        return -1
    else:
        return compare_timestamp(optime1["ts"], optime2["ts"])


class ReplicaSetFixture(interface.ReplFixture, interface._DockerComposeInterface):
    """Fixture which provides JSTests with a replica set to run against."""

    AWAIT_SHARDING_INITIALIZATION_TIMEOUT_SECS = 60

    def __init__(
        self,
        logger,
        job_num,
        fixturelib,
        mongod_executable=None,
        mongod_options=None,
        dbpath_prefix=None,
        preserve_dbpath=False,
        num_nodes=2,
        start_initial_sync_node=False,
        electable_initial_sync_node=False,
        write_concern_majority_journal_default=None,
        auth_options=None,
        replset_config_options=None,
        voting_secondaries=True,
        all_nodes_electable=False,
        use_replica_set_connection_string=None,
        linear_chain=False,
        default_read_concern=None,
        default_write_concern=None,
        shard_logging_prefix=None,
        replicaset_logging_prefix=None,
        replset_name=None,
        use_auto_bootstrap_procedure=None,
        initial_sync_uninitialized_fcv=False,
        hide_initial_sync_node_from_conn_string=False,
        launch_mongot=False,
        load_all_extensions=False,
        router_endpoint_for_mongot: Optional[int] = None,
        disagg_base_config=None,
    ):
        """Initialize ReplicaSetFixture."""

        interface.ReplFixture.__init__(
            self, logger, job_num, fixturelib, dbpath_prefix=dbpath_prefix
        )

        self.mongod_executable = mongod_executable
        self.mongod_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(mongod_options, {})
        )

        self.load_all_extensions = load_all_extensions or self.config.LOAD_ALL_EXTENSIONS
        if self.load_all_extensions:
            self.loaded_extensions = find_and_generate_extension_configs(
                is_evergreen=self.config.EVERGREEN_TASK_ID,
                logger=self.logger,
                mongod_options=self.mongod_options,
            )

        self.preserve_dbpath = preserve_dbpath
        self.start_initial_sync_node = start_initial_sync_node
        self.electable_initial_sync_node = electable_initial_sync_node
        self.write_concern_majority_journal_default = write_concern_majority_journal_default
        self.auth_options = auth_options
        self.replset_config_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(replset_config_options, {})
        )
        self.voting_secondaries = voting_secondaries
        self.all_nodes_electable = all_nodes_electable
        self.use_replica_set_connection_string = use_replica_set_connection_string
        self.default_read_concern = default_read_concern
        self.default_write_concern = default_write_concern
        self.shard_logging_prefix = shard_logging_prefix
        self.replicaset_logging_prefix = replicaset_logging_prefix
        self.num_nodes = num_nodes
        self.replset_name = replset_name
        self.initial_sync_uninitialized_fcv = initial_sync_uninitialized_fcv
        self.hide_initial_sync_node_from_conn_string = hide_initial_sync_node_from_conn_string
        # Used by the enhanced multiversion system to signify multiversion mode.
        # None implies no multiversion run.
        self.fcv = None
        # Used by suites that run search integration tests.
        self.launch_mongot = launch_mongot
        # Used to set --mongoHostAndPort startup option on mongot.
        self.router_endpoint_for_mongot = router_endpoint_for_mongot
        # Use the values given from the command line if they exist for linear_chain and num_nodes.
        linear_chain_option = self.fixturelib.default_if_none(
            self.config.LINEAR_CHAIN, linear_chain
        )
        self.linear_chain = linear_chain_option if linear_chain_option else linear_chain

        # By default, we only use a replica set connection string if all nodes are capable of being
        # elected primary.
        if self.use_replica_set_connection_string is None:
            self.use_replica_set_connection_string = self.all_nodes_electable

        if self.default_write_concern is True:
            self.default_write_concern = self.fixturelib.make_historic(
                {
                    "w": "majority",
                    # Use a "signature" value that won't typically match a value assigned in normal use.
                    # This way the wtimeout set by this override is distinguishable in the server logs.
                    "wtimeout": 5 * 60 * 1000 + 321,  # 300321ms
                }
            )

        # Set the default oplogSize to 511MB.
        self.mongod_options.setdefault("oplogSize", 511)

        self.disagg_base_config = disagg_base_config

        # The dbpath in mongod_options is used as the dbpath prefix for replica set members and
        # takes precedence over other settings. The ShardedClusterFixture uses this parameter to
        # create replica sets and assign their dbpath structure explicitly.
        if "dbpath" in self.mongod_options:
            self._dbpath_prefix = self.mongod_options.pop("dbpath")
        else:
            self._dbpath_prefix = os.path.join(self._dbpath_prefix, self.config.FIXTURE_SUBDIR)

        self.nodes = []
        if "serverless" not in self.mongod_options:
            if not self.replset_name:
                self.replset_name = "rs"
            self.replset_name = self.mongod_options.setdefault("replSet", self.replset_name)
        self.initial_sync_node = None
        self.initial_sync_node_idx = -1
        self.use_auto_bootstrap_procedure = use_auto_bootstrap_procedure
        # The below ports will be set in setup() after the MongoTFixture has been launched.
        self.mongot_port = None
        # mongot_grpc_port is the ingress grpc port on mongot that is configured for the search
        # in community architecture. See setup_mongot_params and MongoDFixture.__init__ for more details.
        self.mongot_grpc_port = None
        # Track the fixture removal [teardown] performed during removeShard testing.
        # This is needed, because we expect the fixture to be in the 'running' state
        # when the evergeen job performs the final teardown. Therefore if the fixture was
        # teared down earlier, it must be skipped during those final checks.
        self.removeshard_teardown_marker = False
        self.removeshard_teardown_mutex = Lock()

    def setup(self):
        """Set up the replica set."""

        if self.disagg_base_config:
            # Wait for primary (first node) to get elected first before
            # starting other nodes, otherwise the election can race.
            self.nodes[0].setup()
            self.nodes[0].await_ready()
            self._await_primary()
            for i in range(1, self.num_nodes):
                self.nodes[i].setup()
                self.nodes[i].await_ready()
            return

        start_node = 0
        if self.use_auto_bootstrap_procedure:
            # We need to wait for the first node to finish auto-bootstrapping so that we can
            # get the auto generated replSet name and update the replSet name of the other mongods with it.
            self.nodes[0].setup()
            self.nodes[0].await_ready()
            self._await_primary()  # Wait for writeable primary (this indicates replSet auto-intiiate finished).

            client = interface.build_client(self.nodes[0], self.auth_options)
            res = client.admin.command("hello")

            self.logger.info(
                f"ReplicaSetFixture using auto generated replSet name {res['setName']} instead of {self.replset_name}"
            )

            self.replset_name = res["setName"]
            self.mongod_options.setdefault("replSet", self.replset_name)
            # The first node should not have the --replSet option because it is auto-bootstrapped.
            for i in range(1, self.num_nodes):
                self.nodes[i].mongod_options["replSet"] = self.replset_name

            start_node = 1

        with ThreadPoolExecutor() as executor:
            tasks = []

            # Version-agnostic options for mongod/s can be set here.
            # Version-specific options should be set in get_version_specific_options_for_mongod()
            # to avoid options for old versions being applied to new Replicaset fixtures.
            # If we are using the auto_bootstrap_procedure we don't need to setup the first node again.
            for i in range(start_node, self.num_nodes):
                tasks.append(executor.submit(self.nodes[i].setup))

            if self.initial_sync_node:
                tasks.append(executor.submit(self.initial_sync_node.setup))

            # Wait for the setup of all nodes to complete
            for task in as_completed(tasks):
                task.result()

        if self.initial_sync_node:
            self.initial_sync_node.await_ready()
            if self.initial_sync_uninitialized_fcv:
                self._pause_initial_sync_at_uninitialized_fcv(self.initial_sync_node)

        if not self.use_auto_bootstrap_procedure:
            # We need only to wait to connect to the first node of the replica set because we first
            # initiate it as a single node replica set.
            self.nodes[0].await_ready()

        # Initiate the replica set.
        members = []
        for i, node in enumerate(self.nodes):
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
            member_config = {
                "_id": self.initial_sync_node_idx,
                "host": self.initial_sync_node.get_internal_connection_string(),
            }
            if not self.electable_initial_sync_node:
                member_config["hidden"] = 1
                member_config["votes"] = 0
                member_config["priority"] = 0

            members.append(member_config)

        repl_config = {"_id": self.replset_name, "protocolVersion": 1}
        client = interface.build_client(self.nodes[0], self.auth_options)

        if (
            client.local.system.replset.count_documents(filter={})
            and not self.use_auto_bootstrap_procedure
        ):
            # Skip initializing the replset if there is an existing configuration.
            # Auto-bootstrapping will automatically create a configuration document but we do not
            # want to skip reconfiguring the replset (which adds the other nodes
            # to the auto-bootstrapped replset).
            self.logger.info("Configuration exists. Skipping initializing the replset.")
            return

        if self.write_concern_majority_journal_default is not None:
            repl_config["writeConcernMajorityJournalDefault"] = (
                self.write_concern_majority_journal_default
            )
        else:
            server_status = client.admin.command({"serverStatus": 1})
            if not server_status["storageEngine"]["persistent"]:
                repl_config["writeConcernMajorityJournalDefault"] = False

        if self.replset_config_options.get("configsvr", False) or (
            self.use_auto_bootstrap_procedure and "shardsvr" not in self.mongod_options
        ):
            repl_config["configsvr"] = True
        if self.replset_config_options.get("settings"):
            replset_settings = self.replset_config_options["settings"]
            repl_config["settings"] = replset_settings

        # Increase the election timeout to 24 hours to prevent spurious elections.
        repl_config.setdefault("settings", {})
        if "electionTimeoutMillis" not in repl_config["settings"]:
            repl_config["settings"]["electionTimeoutMillis"] = 24 * 60 * 60 * 1000

        # Start up a single node replica set then reconfigure to the correct size (if the config
        # contains more than 1 node), so the primary is elected more quickly.
        repl_config["members"] = [members[0]]

        # When this is True, we are running in Antithesis & modify the config to surface more bugs
        if self.config.NOOP_MONGO_D_S_PROCESSES:
            repl_config["settings"]["electionTimeoutMillis"] = 2000
            repl_config["settings"]["chainingAllowed"] = False
            repl_config["settings"]["heartbeatTimeoutSecs"] = 1
            repl_config["settings"]["catchUpTimeoutMillis"] = 0

        if self.use_auto_bootstrap_procedure:
            # Auto-bootstrap already initiates automatically on the first node, but we still need
            # to apply the requested repl_config settings using reconfig.
            self._reconfig_repl_set(client, repl_config)
        else:
            self.logger.info("Issuing replSetInitiate command: %s", repl_config)
            self._initiate_repl_set(client, repl_config)
            self._await_primary()

        if self.fcv is not None:
            # Initiating a replica set with a single node will use "latest" FCV. This will
            # cause IncompatibleServerVersion errors if additional "last-lts" binary version
            # nodes are subsequently added to the set, since such nodes cannot set their FCV to
            # "latest". Therefore, we make sure the primary is "last-lts" FCV before adding in
            # nodes of different binary versions to the replica set.
            client.admin.command(
                {
                    "setFeatureCompatibilityVersion": self.fcv,
                    "fromConfigServer": True,
                }
            )

        if self.nodes[1:]:
            # Wait to connect to each of the secondaries before running the replSetReconfig
            # command.
            for node in self.nodes[1:]:
                node.await_ready()
            # Add in the members one at a time, since non force reconfigs can only add/remove a
            # single voting member at a time.
            for ind in range(2, len(members) + 1):
                self._add_node_to_repl_set(client, repl_config, ind, members)

        self.removeshard_teardown_marker = False

    def _all_mongo_d_s_t(self):
        """Return a list of all `mongo{d,s,t}` `Process` instances in this fixture."""
        nodes = sum([node._all_mongo_d_s_t() for node in self.nodes], [])

        if self.initial_sync_node:
            nodes.extend(self.initial_sync_node._all_mongo_d_s_t())

        return nodes

    def _all_mongots(self):
        """Return a list of all `mongot` `Process` instances in this fixture."""
        return [node.mongot for node in self.nodes]

    def pids(self):
        """:return: all pids owned by this fixture if any."""
        pids = []
        for node in self.nodes:
            pids.extend(node.pids())
        if not pids:
            self.logger.debug("No members running when gathering replicaset fixture pids.")
        return pids

    def _add_node_to_repl_set(self, client, repl_config, member_index, members):
        self.logger.info("Adding in node %d: %s", member_index, members[member_index - 1])
        repl_config["members"] = members[:member_index]
        self._reconfig_repl_set(client, repl_config)

    def _reconfig_repl_set(self, client, repl_config):
        while True:
            try:
                # 'newlyAdded' removal reconfigs could bump the version.
                # Get the current version to be safe.
                curr_version = client.admin.command({"replSetGetConfig": 1})["config"]["version"]
                repl_config["version"] = curr_version + 1

                self.logger.info("Issuing replSetReconfig command: %s", repl_config)
                client.admin.command(
                    {
                        "replSetReconfig": repl_config,
                        "maxTimeMS": self.AWAIT_REPL_TIMEOUT_MINS * 60 * 1000,
                    }
                )
                break
            except pymongo.errors.OperationFailure as err:
                # These error codes may be transient, and so we retry the reconfig with a
                # (potentially) higher config version. We should not receive these codes
                # indefinitely.
                if err.code not in [
                    ReplicaSetFixture._NEW_REPLICA_SET_CONFIGURATION_INCOMPATIBLE,
                    ReplicaSetFixture._CURRENT_CONFIG_NOT_COMMITTED_YET,
                    ReplicaSetFixture._CONFIGURATION_IN_PROGRESS,
                    ReplicaSetFixture._NODE_NOT_FOUND,
                    ReplicaSetFixture._INTERRUPTED_DUE_TO_REPL_STATE_CHANGE,
                    ReplicaSetFixture._INTERRUPTED_DUE_TO_STORAGE_CHANGE,
                ]:
                    msg = (
                        "Operation failure while setting up the " "replica set fixture: {}"
                    ).format(err)
                    self.logger.error(msg)
                    raise self.fixturelib.ServerFailure(msg)

                msg = ("Retrying failed attempt to add new node to fixture: {}").format(err)
                self.logger.error(msg)
                time.sleep(0.1)  # Wait a little bit before trying again.

    def _initiate_repl_set(self, client, repl_config):
        # replSetInitiate (and replSetReconfig) commands can fail with a NodeNotFound error
        # if a heartbeat times out during the quorum check. We retry three times to reduce
        # the chance of failing this way.
        num_initiate_attempts = 3
        for attempt in range(1, num_initiate_attempts + 1):
            try:
                client.admin.command({"replSetInitiate": repl_config})
                break
            except pymongo.errors.OperationFailure as err:
                # Retry on NodeNotFound errors from the "replSetInitiate" command.
                if err.code != ReplicaSetFixture._NODE_NOT_FOUND:
                    msg = (
                        "Operation failure while configuring the " "replica set fixture: {}"
                    ).format(err)
                    self.logger.error(msg)
                    raise self.fixturelib.ServerFailure(msg)

                msg = "replSetInitiate failed attempt {0} of {1} with error: {2}".format(
                    attempt, num_initiate_attempts, err
                )
                self.logger.error(msg)
                if attempt == num_initiate_attempts:
                    msg = "Exceeded number of retries while configuring the replica set fixture"
                    self.logger.error(msg + ".")
                    raise self.fixturelib.ServerFailure(msg)
                time.sleep(5)  # Wait a little bit before trying again.

    def await_last_op_committed(self, timeout_secs=None):
        """Wait for the last majority committed op to be visible."""
        primary_client = interface.build_client(self.get_primary(), self.auth_options)

        primary_optime = get_last_optime(primary_client, self.fixturelib)
        up_to_date_nodes = set()

        def check_rcmaj_optime(client, node):
            """Return True if all nodes have caught up with the primary."""
            res = client.admin.command({"replSetGetStatus": 1})

            if "readConcernMajorityOpTime" not in res["optimes"]:
                # This can be null when the node is starting up.
                return False

            read_concern_majority_optime = res["optimes"]["readConcernMajorityOpTime"]

            if compare_optime(read_concern_majority_optime, primary_optime) >= 0:
                up_to_date_nodes.add(node.port)

            return len(up_to_date_nodes) == len(self.nodes)

        self._await_cmd_all_nodes(
            check_rcmaj_optime, "waiting for last committed optime", timeout_secs
        )

    def await_ready(self):
        """Wait for replica set to be ready."""
        self._await_primary()
        self._await_secondaries()
        self._await_newly_added_removals()
        self._await_stable_recovery_timestamp()
        self._setup_cwrwc_defaults()
        self._await_read_concern_available()
        if self.use_auto_bootstrap_procedure:
            # TODO: Remove this in SERVER-80010.
            self._await_auto_bootstrapped_config_shard()

    def _await_primary(self):
        # Wait for the primary to be elected.
        # Since this method is called at startup we expect the first node to be primary even when
        # self.all_nodes_electable is True.
        primary = self.nodes[0]
        client = primary.mongo_client()
        while True:
            self.logger.info("Waiting for primary on port %d to be elected.", primary.port)
            cmd_result = client.admin.command("isMaster")
            if cmd_result["ismaster"]:
                break
            time.sleep(0.1)  # Wait a little bit before trying again.
        self.logger.info("Primary on port %d successfully elected.", primary.port)

    def _await_secondaries(self):
        # Wait for the secondaries to become available.
        # Since this method is called at startup we expect the nodes 1 to n to be secondaries even
        # when self.all_nodes_electable is True.
        secondaries = self.nodes[1:]
        if self.initial_sync_node and not self.initial_sync_uninitialized_fcv:
            secondaries.append(self.initial_sync_node)

        for secondary in secondaries:
            client = secondary.mongo_client(read_preference=pymongo.ReadPreference.SECONDARY)
            while True:
                self.logger.info(
                    "Waiting for secondary on port %d to become available.", secondary.port
                )
                try:
                    is_secondary = client.admin.command("isMaster")["secondary"]
                    if is_secondary:
                        break
                except pymongo.errors.OperationFailure as err:
                    if err.code != ReplicaSetFixture._INTERRUPTED_DUE_TO_STORAGE_CHANGE:
                        raise
                except pymongo.errors.AutoReconnect:
                    self.logger.info("AutoReconnect exception thrown, retrying...")
                time.sleep(0.1)  # Wait a little bit before trying again.
            self.logger.info("Secondary on port %d is now available.", secondary.port)

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
        primary_client = interface.build_client(self.nodes[0], self.auth_options)

        # All nodes must be in primary/secondary state prior to this point. Perform a majority
        # write to ensure there is a committed operation on the set. The commit point will
        # propagate to all members and trigger a stable checkpoint on all persisted storage engines
        # nodes.
        admin = primary_client.get_database(
            "admin", write_concern=pymongo.write_concern.WriteConcern(w="majority")
        )
        admin.command("appendOplogNote", data={"await_stable_recovery_timestamp": 1})

        for node in self.nodes:
            self.logger.info(
                "Waiting for node on port %d to have a stable recovery timestamp.", node.port
            )
            client = interface.build_client(
                node, self.auth_options, read_preference=pymongo.ReadPreference.SECONDARY
            )

            client_admin = client["admin"]

            while True:
                status = client_admin.command("replSetGetStatus")

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
                        node.port,
                        last_stable_recovery_timestamp,
                    )
                    break
                time.sleep(0.1)  # Wait a little bit before trying again.

    def _should_await_newly_added_removals_longer(self, client):
        """
        Return whether the current replica set config has any 'newlyAdded' fields.

        Return true if the current config is not committed.
        """

        get_config_res = client.admin.command(
            {"replSetGetConfig": 1, "commitmentStatus": True, "$_internalIncludeNewlyAdded": True}
        )
        for member in get_config_res["config"]["members"]:
            if "newlyAdded" in member:
                self.logger.info(
                    "Waiting longer for 'newlyAdded' removals, "
                    + "member %d is still 'newlyAdded'",
                    member["_id"],
                )
                return True
        if not get_config_res["commitmentStatus"]:
            self.logger.info(
                "Waiting longer for 'newlyAdded' removals, " + "config is not yet committed"
            )
            return True

        return False

    def _await_newly_added_removals(self):
        """
        Wait for all 'newlyAdded' fields to be removed from the replica set config.

        Additionally, wait for that config to be committed, and for the in-memory
        and on-disk configs to match.
        """

        self.logger.info("Waiting to remove all 'newlyAdded' fields")
        primary = self.get_primary()
        client = interface.build_client(primary, self.auth_options)
        while self._should_await_newly_added_removals_longer(client):
            time.sleep(0.1)  # Wait a little bit before trying again.
        self.logger.info("All 'newlyAdded' fields removed")

    def _setup_cwrwc_defaults(self):
        """Set up the cluster-wide read/write concern defaults."""
        if self.default_read_concern is None and self.default_write_concern is None:
            return
        cmd = {"setDefaultRWConcern": 1}
        if self.default_read_concern is not None:
            cmd["defaultReadConcern"] = self.default_read_concern
        if self.default_write_concern is not None:
            cmd["defaultWriteConcern"] = self.default_write_concern
        primary = self.nodes[0]
        primary.mongo_client().admin.command(cmd)

    # TODO: Remove this in SERVER-80010.
    def _await_auto_bootstrapped_config_shard(self):
        connection_string = self.get_driver_connection_url()
        self.logger.info("Waiting for %s to auto-bootstrap as a config shard...", connection_string)

        deadline = time.time() + ReplicaSetFixture.AWAIT_SHARDING_INITIALIZATION_TIMEOUT_SECS

        def timeout_occurred():
            return deadline - time.time() <= 0.0

        while True:
            client = interface.build_client(self.get_primary(), self.auth_options)
            config_shard_count = client.get_database("config").command(
                {"count": "shards", "query": {"_id": "config"}}
            )

            if config_shard_count["n"] == 1:
                break

            if timeout_occurred():
                port = self.get_primary().port
                raise self.fixturelib.ServerFailure(
                    "mongod on port: {} failed waiting for auto-bootstrapped config shard success after {} seconds".format(
                        port, interface.Fixture.AWAIT_READY_TIMEOUT_SECS
                    )
                )
            time.sleep(0.1)

        self.logger.info(
            "%s successfully auto-bootstrapped as a config shard...", connection_string
        )

    def _check_initial_sync_node_has_uninitialized_fcv(self, initial_sync_node):
        sync_node_conn = initial_sync_node.mongo_client()
        self.logger.info("Checking that initial sync node has uninitialized fcv")
        try:
            fcv = sync_node_conn.admin.command(
                {"getParameter": 1, "featureCompatibilityVersion": 1}
            )

            msg = "Initial sync node should have an uninitialized FCV, but got fcv: " + str(fcv)
            raise self.fixturelib.ServerFailure(msg)
        except pymongo.errors.OperationFailure as err:
            if err.code == 258:  # codeName == 'UnknownFeatureCompatibilityVersion'
                return
            raise

    def _pause_initial_sync_at_uninitialized_fcv(self, initial_sync_node):
        failpointOnCmd = {
            "configureFailPoint": "initialSyncHangAfterResettingFCV",
            "mode": "alwaysOn",
        }
        sync_node_conn = initial_sync_node.mongo_client()
        self.logger.info("Pausing initial sync at failpoint")
        sync_node_conn.admin.command(failpointOnCmd)
        self._check_initial_sync_node_has_uninitialized_fcv(initial_sync_node)

    def _unpause_initial_sync(self, initial_sync_node):
        failpoint_off_cmd = {
            "configureFailPoint": "initialSyncHangAfterResettingFCV",
            "mode": "off",
        }
        self.logger.info("Unpausing initial sync")
        sync_node_conn = initial_sync_node.mongo_client()
        sync_node_conn.admin.command(failpoint_off_cmd)

    def _do_teardown(self, finished=False, mode=None):
        self.logger.info("Stopping all members of the replica set '%s'...", self.replset_name)

        if finished and self.load_all_extensions and self.loaded_extensions:
            delete_extension_configs(self.loaded_extensions, self.logger)

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.info(
                "All members of the replica set were expected to be running, " "but weren't."
            )

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        if self.initial_sync_node:
            if self.initial_sync_uninitialized_fcv:
                self._check_initial_sync_node_has_uninitialized_fcv(self.initial_sync_node)
                self._unpause_initial_sync(self.initial_sync_node)
            teardown_handler.teardown(self.initial_sync_node, "initial sync node", mode=mode)

        with ThreadPoolExecutor() as executor:
            tasks = []
            # Terminate the secondaries first to reduce noise in the logs.
            for node in reversed(self.nodes):
                tasks.append(
                    executor.submit(
                        teardown_handler.teardown,
                        node,
                        "replica set member on port %d" % node.port,
                        mode,
                    )
                )
            # Wait for all the teardown tasks to complete
            for task in as_completed(tasks):
                task.result()

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped all members of the replica set.")
        else:
            self.logger.error("Stopping the replica set fixture failed.")
            raise self.fixturelib.ServerFailure(teardown_handler.get_error_message())

    def is_running(self):
        """Return True if all nodes in the replica set are running."""
        running = all(node.is_running() for node in self.nodes)

        if self.initial_sync_node:
            running = self.initial_sync_node.is_running() and running

        return running

    def get_primary(self, timeout_secs=30):
        """Return the primary from a replica set."""
        if not self.all_nodes_electable:
            # The primary is always the first element of the 'nodes' list because all other members
            # of the replica set are configured with priority=0.
            return self.nodes[0]

        def is_primary(client, node):
            """Return if `node` is master."""
            is_master = client.admin.command("isMaster")["ismaster"]
            if is_master:
                self.logger.info(
                    "The node on port %d is primary of replica set '%s'",
                    node.port,
                    self.replset_name,
                )
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

        all_nodes = self.nodes.copy()
        if self.electable_initial_sync_node:
            all_nodes.append(self.initial_sync_node)

        while True:
            for node in all_nodes:
                now = time.time()
                if (now - start) >= timeout_secs:
                    msg = "Timed out while {} for replica set '{}'.".format(msg, self.replset_name)
                    self.logger.error(msg)
                    raise self.fixturelib.ServerFailure(msg)

                try:
                    if node.port not in clients:
                        clients[node.port] = interface.build_client(node, self.auth_options)

                    if fn(clients[node.port], node):
                        return node

                except pymongo.errors.AutoReconnect:
                    # AutoReconnect exceptions may occur if the primary stepped down since PyMongo
                    # last contacted it. We'll just try contacting the node again in the next round
                    # of isMaster requests.
                    continue

    def _await_read_concern_available_on_node(self, client):
        retry_time_secs = self.AWAIT_REPL_TIMEOUT_MINS * 60
        retry_start_time = time.time()
        while "$clusterTime" not in client.admin.command("ping"):
            if time.time() - retry_start_time > retry_time_secs:  # Check for timeout.
                raise self.fixturelib.ServerFailure(
                    "$clusterTime did not appear in a command response from the node on port {} in "
                    "{} seconds.".format(client.port, retry_time_secs)
                )
            time.sleep(0.1)  # Wait a little bit before trying again.

    def _await_read_concern_available(self):
        """Await $clusterTime to appear in command responses to satisfy readConcern guarantees."""
        self.logger.info("Waiting for read concern to become available on all nodes")
        for node in self.nodes:
            self.logger.info(
                "Waiting for node on port %d to have readConcern guarantees available.", node.port
            )
            self._await_read_concern_available_on_node(
                interface.build_client(
                    node, self.auth_options, read_preference=pymongo.ReadPreference.SECONDARY
                )
            )
        self.logger.info("Read concern is available on all nodes")

    def stop_primary(self, primary, background_reconfig, should_kill):
        """Stop the primary node method."""
        # Check that the fixture is still running before stepping down or killing the primary.
        # This ensures we still detect some cases in which the fixture has already crashed.
        if not self.is_running():
            raise self.fixturelib.ServerFailure(
                "ReplicaSetFixture {} expected to be running in"
                " ContinuousStepdown, but wasn't.".format(self.replset_name)
            )

        # If we're running with background reconfigs, it's possible to be in a scenario
        # where we kill a necessary voting node (i.e. in a 5 node repl set), only 2 are
        # voting. In this scenario, we want to avoid killing the primary because no
        # secondary can step up.
        if background_reconfig:
            # stagger the kill thread so that it runs a little after the reconfig thread
            time.sleep(1)
            voting_members = self.get_voting_members()

            self.logger.info("Current voting members: %s", voting_members)

            if len(voting_members) <= 3:
                # Do not kill or terminate the primary if we don't have enough voting nodes to
                # elect a new primary.
                return False

        action = "Killing" if should_kill else "Terminating"
        self.logger.info(
            "%s the primary on port %d of replica set '%s'.",
            action,
            primary.port,
            self.replset_name,
        )

        # We send the mongod process the signal to exit but don't immediately wait for it to
        # exit because clean shutdown may take a while and we want to restore write availability
        # as quickly as possible.
        teardown_mode = (
            interface.TeardownMode.KILL if should_kill else interface.TeardownMode.TERMINATE
        )
        primary.mongod.stop(mode=teardown_mode)
        return True

    def change_version_and_restart_node(self, primary, auth_options):
        """
        Select Secondary for stepUp.

        Ensure its version is different to that
        of the old primary; change the version of the Secondary is needed.
        """

        def get_chosen_node_from_replsetstatus(status_member_infos):
            max_optime = None
            chosen_index = None
            # We always select the secondary with highest optime to setup.
            for member_info in status_member_infos:
                if member_info.get("self", False):
                    # Ignore self, which is the old primary and not eligible
                    # to be re-elected in downgrade multiversion cluster.
                    continue
                optime_dict = member_info["optime"]
                if max_optime is None:
                    chosen_index = member_info["_id"]
                    max_optime = optime_dict
                else:
                    if compare_optime(optime_dict, max_optime) > 0:
                        chosen_index = member_info["_id"]
                        max_optime = optime_dict

                if chosen_index is None or max_optime is None:
                    raise self.fixturelib.ServerFailure(
                        "Failed to find a secondary eligible for "
                        f"election; index: {chosen_index}, optime: {max_optime}"
                    )

                return self.nodes[chosen_index]

        primary_client = interface.build_client(primary, auth_options)
        retry_time_secs = self.AWAIT_REPL_TIMEOUT_MINS * 60
        retry_start_time = time.time()

        while True:
            member_infos = primary_client.admin.command({"replSetGetStatus": 1})["members"]
            chosen_node = get_chosen_node_from_replsetstatus(member_infos)

            if chosen_node.change_version_if_needed(primary):
                self.logger.info(
                    "Waiting for the chosen secondary on port %d of replica set '%s' to exit.",
                    chosen_node.port,
                    self.replset_name,
                )

                teardown_mode = interface.TeardownMode.TERMINATE
                chosen_node.mongod.stop(mode=teardown_mode)
                chosen_node.mongod.wait()

                self.logger.info(
                    "Attempting to restart the chosen secondary on port %d of replica set '%s'.",
                    chosen_node.port,
                    self.replset_name,
                )

                chosen_node.setup()
                self.logger.info(interface.create_fixture_table(self))
                chosen_node.await_ready()

            if self.stepup_node(chosen_node, auth_options):
                break

            if time.time() - retry_start_time > retry_time_secs:
                raise self.fixturelib.ServerFailure(
                    "The old primary on port {} of replica set {} did not step up in"
                    " {} seconds.".format(chosen_node.port, self.replset_name, retry_time_secs)
                )

        return chosen_node

    def stepup_node(self, node, auth_options):
        """Try to step up the given node; return whether the attempt was successful."""
        try:
            self.logger.info(
                "Attempting to step up the chosen secondary on port %d of replica set '%s'.",
                node.port,
                self.replset_name,
            )
            client = interface.build_client(node, auth_options)
            client.admin.command("replSetStepUp")
            return True
        except pymongo.errors.OperationFailure:
            # OperationFailure exceptions are expected when the election attempt fails due to
            # not receiving enough votes. This can happen when the 'chosen' secondary's opTime
            # is behind that of other secondaries. We handle this by attempting to elect a
            # different secondary.
            self.logger.info(
                "Failed to step up the secondary on port %d of replica set '%s'.",
                node.port,
                self.replset_name,
            )
            return False
        except pymongo.errors.AutoReconnect:
            # It is possible for a replSetStepUp to fail with AutoReconnect if that node goes
            # into Rollback (which causes it to close any open connections).
            return False

    def restart_node(self, chosen, temporary_flags={}):
        """Restart the new step up node."""
        self.logger.info(
            "Waiting for the old primary on port %d of replica set '%s' to exit.",
            chosen.port,
            self.replset_name,
        )

        exit_code = chosen.mongod.wait()
        # This function is called after stop_primary() which could kill or cleanly shutdown the
        # process. We therefore also allow an exit code of -9.
        if exit_code in (0, -interface.TeardownMode.KILL.value):
            self.logger.info("Successfully stopped the mongod on port {:d}.".format(chosen.port))
        else:
            self.logger.warning(
                "Stopped the mongod on port {:d}. " "Process exited with code {:d}.".format(
                    chosen.port, exit_code
                )
            )
            raise self.fixturelib.ServerFailure(
                "mongod on port {:d} with pid {:d} exited with code {:d}".format(
                    chosen.port, chosen.mongod.pid, exit_code
                )
            )

        self.logger.info(
            "Attempting to restart the old primary on port %d of replica set '%s'.",
            chosen.port,
            self.replset_name,
        )

        # Restart the mongod on the old primary and wait until we can contact it again. Keep the
        # original preserve_dbpath to restore after restarting the mongod.
        original_preserve_dbpath = chosen.preserve_dbpath
        chosen.preserve_dbpath = True
        try:
            chosen.setup(temporary_flags=temporary_flags)
            self.logger.info(interface.create_fixture_table(self))
            chosen.await_ready()
        finally:
            chosen.preserve_dbpath = original_preserve_dbpath

    def get_secondaries(self):
        """Return a list of secondaries from the replica set."""
        primary = self.get_primary()
        return [node for node in self.nodes if node.port != primary.port]

    def get_secondary_indices(self):
        """Return a list of secondary indices from the replica set."""
        primary = self.get_primary()
        return [index for index, node in enumerate(self.nodes) if node.port != primary.port]

    def get_voting_members(self):
        """Return the number of voting nodes in the replica set."""
        primary = self.get_primary()
        client = primary.mongo_client()

        members = client.admin.command({"replSetGetConfig": 1})["config"]["members"]
        voting_members = [member["host"] for member in members if member["votes"] == 1]

        return voting_members

    def get_initial_sync_node(self):
        """Return initial sync node from the replica set."""
        return self.initial_sync_node

    def set_fcv(self, fcv):
        """Set the fcv used by this fixtures."""
        self.fcv = fcv

    def install_mongod(self, mongod):
        """Install a mongod node. Called by a builder."""
        self.nodes.append(mongod)

    def get_options_for_mongod(self, index):
        """Return options that may be passed to a mongod."""
        mongod_options = self.mongod_options.copy()

        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "node{}".format(index))
        mongod_options["set_parameters"] = mongod_options.get(
            "set_parameters", self.fixturelib.make_historic({})
        ).copy()

        if index == 0 and self.use_auto_bootstrap_procedure:
            del mongod_options["replSet"]

        if self.linear_chain and index > 0:
            self.mongod_options["set_parameters"]["failpoint.forceSyncSourceCandidate"] = (
                self.fixturelib.make_historic(
                    {
                        "mode": "alwaysOn",
                        "data": {
                            "hostAndPort": self.nodes[index - 1].get_internal_connection_string()
                        },
                    }
                )
            )
        return mongod_options

    def get_logger_for_mongod(self, index):
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

        if self.shard_logging_prefix is not None:
            node_name = f"{self.shard_logging_prefix}:{node_name}"
            return self.fixturelib.new_fixture_node_logger(
                "ShardedClusterFixture", self.job_num, node_name
            )

        if self.replicaset_logging_prefix is not None:
            node_name = f"{self.replicaset_logging_prefix}:{node_name}"

        return self.fixturelib.new_fixture_node_logger(
            self.__class__.__name__, self.job_num, node_name
        )

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        conn_strs = [node.get_internal_connection_string() for node in self.nodes]

        # ReplicaSetFixture sets initial sync nodes as hidden,
        # which causes a mismatch if the replica set is added to the sharded cluster
        # through addShard, because the replica set's internal connection string normally
        # does include the initial sync node, but the list of hosts in the replica set from
        # running `hello`/`isMaster` does not include it. Setting hide_initial_sync_node_from_conn_string
        # to True force-hides it from the connection string.
        if self.initial_sync_node and not self.hide_initial_sync_node_from_conn_string:
            conn_strs.append(self.initial_sync_node.get_internal_connection_string())
        return self.replset_name + "/" + ",".join(conn_strs)

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        for node in self.nodes:
            output += node.get_node_info()
        if self.initial_sync_node:
            output += self.initial_sync_node.get_node_info()
        return output

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        if self.use_replica_set_connection_string:
            # We use a replica set connection string when all nodes are electable because we
            # anticipate the client will want to gracefully handle any failovers.
            conn_strs = [node.get_internal_connection_string() for node in self.nodes]
            if self.initial_sync_node:
                conn_strs.append(self.initial_sync_node.get_internal_connection_string())
            return "mongodb://" + ",".join(conn_strs) + "/?replicaSet=" + self.replset_name
        else:
            # We return a direct connection to the expected primary when only the first node is
            # electable because we want the client to error out if a stepdown occurs.
            return self.nodes[0].get_driver_connection_url()

    def write_historic(self, obj):
        """Convert the obj to a record to track history."""
        self.fixturelib.make_historic(obj)


def get_last_optime(client, fixturelib):
    """Get the latest optime.

    This function is derived from _getLastOpTime() in ReplSetTest.
    """
    repl_set_status = client.admin.command({"replSetGetStatus": 1})
    conn_status = [m for m in repl_set_status["members"] if "self" in m][0]
    optime = conn_status["optime"]

    optime_is_empty = False

    if isinstance(optime, bson.Timestamp):  # PV0
        optime_is_empty = optime == bson.Timestamp(0, 0)
    else:  # PV1
        optime_is_empty = optime["ts"] == bson.Timestamp(0, 0) and optime["t"] == -1

    if optime_is_empty:
        raise fixturelib.ServerFailure(
            "Uninitialized opTime being reported by {addr[0]}:{addr[1]}: {repl_set_status}".format(
                addr=client.address, repl_set_status=repl_set_status
            )
        )

    return optime
