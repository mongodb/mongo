"""Fixture for testing shard split operations."""

import time
import os.path
import threading
import shutil

import pymongo
from bson.objectid import ObjectId

import buildscripts.resmokelib.testing.fixtures.interface as interface


def _is_replica_set_fixture(fixture):
    """Determine whether the passed in fixture is a ReplicaSetFixture."""
    return hasattr(fixture, 'replset_name')


def _teardown_and_clean_node(node):
    """Teardown the provided node and remove its data directory."""
    node.teardown(finished=True)
    # Remove the data directory for the node to prevent unbounded disk space utilization.
    shutil.rmtree(node.get_dbpath_prefix(), ignore_errors=False)


class ShardSplitFixture(interface.MultiClusterFixture):
    """Fixture which provides JSTests with a replica set and recipient nodes to run splits against."""

    AWAIT_REPL_TIMEOUT_MINS = 5
    AWAIT_REPL_TIMEOUT_FOREVER_MINS = 24 * 60

    def __init__(
            self,
            logger,
            job_num,
            fixturelib,
            common_mongod_options=None,
            per_mongod_options=None,
            dbpath_prefix=None,
            preserve_dbpath=False,
            num_nodes_per_replica_set=2,
            auth_options=None,
            replset_config_options=None,
            mixed_bin_versions=None,
    ):
        """Initialize ShardSplitFixture with different options for the replica set processes."""
        interface.MultiClusterFixture.__init__(self, logger, job_num, fixturelib,
                                               dbpath_prefix=dbpath_prefix)
        self.__lock = threading.Lock()

        self.common_mongod_options = self.fixturelib.default_if_none(common_mongod_options, {})
        self.per_mongod_options = self.fixturelib.default_if_none(per_mongod_options, {})
        self.dbpath_prefix = dbpath_prefix
        self.preserve_dbpath = preserve_dbpath
        self.auth_options = auth_options
        self.replset_config_options = self.fixturelib.default_if_none(replset_config_options, {})
        self.mixed_bin_versions = self.fixturelib.default_if_none(mixed_bin_versions,
                                                                  self.config.MIXED_BIN_VERSIONS)
        self.num_nodes_per_replica_set = num_nodes_per_replica_set if num_nodes_per_replica_set \
            else self.config.NUM_REPLSET_NODES

        self.fixtures = []
        self._can_teardown_retired_donor_rs = threading.Event()
        # By default, we can always tear down the retired donor rs
        self._can_teardown_retired_donor_rs.set()

        # Make the initial donor replica set
        donor_rs_name = "rs0"
        mongod_options = self.common_mongod_options.copy()
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, donor_rs_name)
        mongod_options["serverless"] = True

        self.fixtures.append(
            self.fixturelib.make_fixture(
                "ReplicaSetFixture", self.logger, self.job_num, mongod_options=mongod_options,
                preserve_dbpath=self.preserve_dbpath, num_nodes=self.num_nodes_per_replica_set,
                auth_options=self.auth_options, replset_config_options=self.replset_config_options,
                mixed_bin_versions=self.mixed_bin_versions, replicaset_logging_prefix=donor_rs_name,
                all_nodes_electable=True, replset_name=donor_rs_name))

        # Ensure that all nodes are only ever run on the same deterministic set of ports, this
        # makes it easier to reroute in the jstest overrides
        self._port_index = 0
        self._ports = [[node.port for node in self.get_donor_rs().nodes],
                       [
                           self.fixturelib.get_next_port(self.job_num)
                           for _ in range(self.num_nodes_per_replica_set)
                       ]]

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = []
        with self.__lock:
            for fixture in self.fixtures:
                out.extend(fixture.pids())
        if not out:
            self.logger.debug('No fixtures when gathering pids.')
        return out

    def setup(self):
        """Set up the replica sets."""
        # Don't take the lock because we don't expect setup to be called while the
        # ContinuousShardSplit hook is running, which is the only thing that can modify
        # self.fixtures. We don't want to take the lock because it would be held while starting
        # mongod instances, which is prone to hanging and could cause other functions which take
        # the lock to hang.
        for fixture in self.fixtures:
            fixture.setup()

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        # Don't take the lock because we don't expect await_ready to be called while the
        # ContinuousShardSplit hook is running, which is the only thing that can modify
        # self.fixtures. We don't want to take the lock because it would be held while waiting for
        # the donor to initiate which may take a long time.
        for fixture in self.fixtures:
            fixture.await_ready()

    def _do_teardown(self, mode=None):
        """Shut down the replica sets."""
        self.logger.info("Stopping all replica sets...")

        running_at_start = self.is_running()
        if not running_at_start:
            self.logger.warning("Donor replica set expected to be running, but wasn't.")

        teardown_handler = interface.FixtureTeardownHandler(self.logger)

        # Don't take the lock because we don't expect teardown to be called while the
        # ContinuousShardSplit hook is running, which is the only thing that can modify
        # self.fixtures. Tearing down may take a long time, so taking the lock during that process
        # might result in hangs in other functions which need to take the lock.
        for fixture in reversed(self.fixtures):
            type_name = f"replica set '{fixture.replset_name}'" if _is_replica_set_fixture(
                fixture) else f"standalone on port {fixture.port}"
            teardown_handler.teardown(fixture, type_name, mode=mode)

        if teardown_handler.was_successful():
            self.logger.info("Successfully stopped donor replica set and all recipient nodes.")
        else:
            self.logger.error("Stopping the fixture failed.")
            raise self.fixturelib.ServerFailure(teardown_handler.get_error_message())

    def is_running(self):
        """Return true if all replica sets are still operating."""
        # This method is most importantly used in between test runs in job.py to determine if a
        # fixture has crashed between test invocations. We return the `is_running` status of the
        # donor here, instead of all fixtures, some of which may not have been started yet.
        return self.get_donor_rs().is_running()

    def get_internal_connection_string(self):
        """Return the internal connection string to the replica set that currently starts out owning the data."""
        donor_rs = self.get_donor_rs()
        if not donor_rs:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")
        return donor_rs.get_internal_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL to the replica set that currently starts out owning the data."""
        donor_rs = self.get_donor_rs()
        if not donor_rs:
            raise ValueError("Must call setup() before calling get_driver_connection_url")
        return donor_rs.get_driver_connection_url()

    def get_node_info(self):
        """Return a list of dicts of NodeInfo objects."""
        output = []
        with self.__lock:
            for fixture in self.fixtures:
                output += fixture.get_node_info()
        return output

    def get_independent_clusters(self):
        """Return the replica sets involved in the tenant migration."""
        with self.__lock:
            return self.fixtures.copy()

    def get_donor_rs(self):
        """:return the donor replica set."""
        with self.__lock:
            donor_rs = next(iter(self.fixtures), None)
            if donor_rs and not _is_replica_set_fixture(donor_rs):
                raise ValueError("Invalid configuration, donor_rs is not a ReplicaSetFixture")
            return donor_rs

    def get_recipient_nodes(self):
        """:return the recipient nodes for the current split operation."""
        with self.__lock:
            return self.fixtures[1:]

    def _create_client(self, fixture, **kwargs):
        return fixture.mongo_client(
            username=self.auth_options["username"], password=self.auth_options["password"],
            authSource=self.auth_options["authenticationDatabase"],
            authMechanism=self.auth_options["authenticationMechanism"], **kwargs)

    def add_recipient_nodes(self, recipient_set_name, recipient_tag_name=None):
        """Build recipient nodes, and reconfig them into the donor as non-voting members."""
        recipient_tag_name = recipient_tag_name or "recipientNode"
        donor_rs_name = self.get_donor_rs().replset_name

        self.logger.info(
            f"Adding {self.num_nodes_per_replica_set} recipient nodes to donor replica set '{donor_rs_name}'."
        )

        with self.__lock:
            self._port_index ^= 1  # Toggle the set of mongod ports between index 0 and 1
            for i in range(self.num_nodes_per_replica_set):
                mongod_logger = self.fixturelib.new_fixture_node_logger(
                    "MongoDFixture", self.job_num, f"{recipient_set_name}:node{i}")

                mongod_options = self.common_mongod_options.copy()
                # Even though these nodes are not starting in a replica set, we structure their
                # files on disk as if they were already part of the new recipient set. This makes
                # logging and cleanup easier.
                mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, recipient_set_name,
                                                        "node{}".format(i))
                mongod_options["set_parameters"] = mongod_options.get(
                    "set_parameters", self.fixturelib.make_historic({})).copy()
                mongod_options["serverless"] = True
                mongod_port = self._ports[self._port_index][i]

                self.fixtures.append(
                    self.fixturelib.make_fixture(
                        "MongoDFixture", mongod_logger, self.job_num, mongod_options=mongod_options,
                        dbpath_prefix=self.dbpath_prefix, preserve_dbpath=self.preserve_dbpath,
                        port=mongod_port))

        recipient_nodes = self.get_recipient_nodes()
        for recipient_node in recipient_nodes:
            recipient_node.setup()
            recipient_node.await_ready()

        # Reconfig the donor to add the recipient nodes as non-voting members
        donor_client = self._create_client(self.get_donor_rs())

        while True:
            try:
                repl_config = donor_client.admin.command({"replSetGetConfig": 1})["config"]
                repl_members = repl_config["members"]

                for recipient_node in recipient_nodes:
                    # It is possible for the reconfig below to fail with a retryable error code like
                    # 'InterruptedDueToReplStateChange'. In these cases, we need to run the reconfig
                    # again, but some or all of the recipient nodes might have already been added to
                    # the member list. Only add recipient nodes which have not yet been added on a
                    # retry.
                    recipient_host = recipient_node.get_internal_connection_string()
                    recipient_entry = {
                        "host": recipient_host, "votes": 0, "priority": 0, "hidden": True,
                        "tags": {recipient_tag_name: str(ObjectId())}
                    }
                    member_exists = False
                    for index, member in enumerate(repl_members):
                        if member["host"] == recipient_host:
                            repl_members[index] = recipient_entry
                            member_exists = True

                    if not member_exists:
                        repl_members.append(recipient_entry)

                # Re-index all members from 0
                for idx, member in enumerate(repl_members):
                    member["_id"] = idx

                # Prepare the new config
                repl_config["version"] = repl_config["version"] + 1
                repl_config["members"] = repl_members

                self.logger.info(
                    f"Reconfiguring donor replica set to add non-voting recipient nodes: {repl_config}"
                )
                donor_client.admin.command({
                    "replSetReconfig": repl_config,
                    "maxTimeMS": self.AWAIT_REPL_TIMEOUT_MINS * 60 * 1000
                })

                # Wait for recipient nodes to become secondaries
                self._await_recipient_nodes()
                return
            except pymongo.errors.ConnectionFailure as err:
                self.logger.info(
                    f"Retrying adding recipient nodes on replica set '{donor_rs_name}' after error: {str(err)}."
                )
                continue

    def _await_recipient_nodes(self):
        """Wait for recipient nodes to become available."""
        recipient_nodes = self.get_recipient_nodes()
        for recipient_node in recipient_nodes:
            recipient_client = self._create_client(recipient_node,
                                                   read_preference=pymongo.ReadPreference.SECONDARY)
            while True:
                self.logger.info(
                    f"Waiting for secondary on port {recipient_node.port} to become available.")
                try:
                    is_secondary = recipient_client.admin.command("isMaster")["secondary"]
                    if is_secondary:
                        break
                except pymongo.errors.OperationFailure as err:
                    if err.code != ShardSplitFixture._INTERRUPTED_DUE_TO_STORAGE_CHANGE:
                        raise
                except pymongo.errors.ConnectionFailure:
                    pass

                time.sleep(0.1)  # Wait a little bit before trying again.
            self.logger.info(f"Secondary on port {recipient_node.port} is now available.")

    def remove_recipient_nodes(self, recipient_tag_name=None):
        """Remove recipient nodes from the donor."""
        recipient_tag_name = recipient_tag_name or "recipientNode"
        donor_rs_name = self.get_donor_rs().replset_name
        recipient_nodes = self.get_recipient_nodes()
        with self.__lock:
            # Reset the port-set, so we select the same ports next time.
            self._port_index ^= 1

            # Remove the recipient nodes from the internal fixture list.
            donor_rs = next(iter(self.fixtures), None)
            if donor_rs and not _is_replica_set_fixture(donor_rs):
                raise ValueError("Invalid configuration, donor_rs is not a ReplicaSetFixture")
            self.fixtures = [donor_rs]

        donor_client = self._create_client(self.get_donor_rs())
        while True:
            try:
                repl_config = donor_client.admin.command({"replSetGetConfig": 1})["config"]
                repl_members = [
                    member for member in repl_config["members"]
                    if not 'tags' in member or not recipient_tag_name in member["tags"]
                ]

                # Re-index all members from 0
                for idx, member in enumerate(repl_members):
                    member["_id"] = idx

                # Prepare the new config
                repl_config["version"] = repl_config["version"] + 1
                repl_config["members"] = repl_members

                # It's possible that the recipient config has been removed in a previous remove attempt.
                if "recipientConfig" in repl_config:
                    del repl_config["recipientConfig"]

                self.logger.info(
                    f"Reconfiguring donor '{donor_rs_name}' to remove recipient nodes: {repl_config}"
                )
                donor_client.admin.command({
                    "replSetReconfig": repl_config,
                    "maxTimeMS": self.AWAIT_REPL_TIMEOUT_MINS * 60 * 1000
                })
                break
            except pymongo.errors.ConnectionFailure as err:
                self.logger.info(
                    f"Retrying removing recipient nodes from donor '{donor_rs_name}' after error: {str(err)}."
                )
                continue

        self.logger.info("Tearing down recipient nodes and removing data directories.")
        for recipient_node in reversed(recipient_nodes):
            _teardown_and_clean_node(recipient_node)

    def replace_donor_with_recipient(self, recipient_set_name):
        """Replace the current donor with the newly initiated recipient."""
        self.logger.info(
            f"Making new donor replica set '{recipient_set_name}' from existing recipient nodes.")
        mongod_options = self.common_mongod_options.copy()
        mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, recipient_set_name)
        mongod_options["serverless"] = True
        new_donor_rs = self.fixturelib.make_fixture(
            "ReplicaSetFixture", self.logger, self.job_num, mongod_options=mongod_options,
            preserve_dbpath=self.preserve_dbpath, num_nodes=self.num_nodes_per_replica_set,
            auth_options=self.auth_options, replset_config_options=self.replset_config_options,
            mixed_bin_versions=self.mixed_bin_versions,
            replicaset_logging_prefix=recipient_set_name, all_nodes_electable=True,
            replset_name=recipient_set_name, existing_nodes=self.get_recipient_nodes())

        new_donor_rs.get_primary()  # Await an election of a new donor primary

        self.logger.info("Replacing internal fixtures with new donor replica set.")
        retired_donor_rs = self.get_donor_rs()
        with self.__lock:
            self.fixtures = [new_donor_rs]

        self._can_teardown_retired_donor_rs.wait()
        self.logger.info(f"Retiring old donor replica set '{retired_donor_rs.replset_name}'.")
        _teardown_and_clean_node(retired_donor_rs)

    def enter_step_down(self):
        """Called by the ContinuousStepDown hook to indicate that we are stepping down."""
        self.logger.info("Entering stepdown, preventing donor from being retired.")
        self._can_teardown_retired_donor_rs.clear()

    def exit_step_down(self):
        """Called by the ContinuousStepDown hook to indicate that we are done stepping down."""
        self.logger.info("Exiting stepdown, donor can now be retired.")
        self._can_teardown_retired_donor_rs.set()
