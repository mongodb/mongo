"""Standalone mongod fixture for executing JSTests against."""

import os
import os.path
import time

import pymongo
import pymongo.errors

from buildscripts.resmokelib import config
from buildscripts.resmokelib import core
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.utils.history import make_historic
from buildscripts.resmokelib.testing.fixtures import interface
from buildscripts.resmokelib.multiversionconstants import LAST_LTS_MONGOD_BINARY

# The below parameters define the default 'logComponentVerbosity' object passed to mongod processes
# started either directly via resmoke or those that will get started by the mongo shell. We allow
# this default to be different for tests run locally and tests run in Evergreen. This allows us, for
# example, to keep log verbosity high in Evergreen test runs without polluting the logs for
# developers running local tests.

# The default verbosity setting for any tests that are not started with an Evergreen task id. This
# will apply to any tests run locally.
DEFAULT_MONGOD_LOG_COMPONENT_VERBOSITY = make_historic({
    "replication": {"rollback": 2}, "sharding": {"migration": 2}, "transaction": 4,
    "tenantMigration": 4
})

DEFAULT_LAST_LTS_MONGOD_LOG_COMPONENT_VERBOSITY = make_historic(
    {"replication": {"rollback": 2}, "transaction": 4})

# The default verbosity setting for any mongod processes running in Evergreen i.e. started with an
# Evergreen task id.
DEFAULT_EVERGREEN_MONGOD_LOG_COMPONENT_VERBOSITY = make_historic({
    "replication": {"election": 4, "heartbeats": 2, "initialSync": 2, "rollback": 2},
    "sharding": {"migration": 2}, "storage": {"recovery": 2}, "transaction": 4, "tenantMigration": 4
})

# The default verbosity setting for any last-lts mongod processes running in Evergreen i.e. started
# with an Evergreen task id.
DEFAULT_EVERGREEN_LAST_LTS_MONGOD_LOG_COMPONENT_VERBOSITY = make_historic({
    "replication": {"election": 4, "heartbeats": 2, "initialSync": 2, "rollback": 2},
    "storage": {"recovery": 2}, "transaction": 4
})


class MongoDFixture(interface.Fixture):
    """Fixture which provides JSTests with a standalone mongod to run against."""

    AWAIT_READY_TIMEOUT_SECS = 300

    def __init__(  # pylint: disable=too-many-arguments
            self, logger, job_num, mongod_executable=None, mongod_options=None, dbpath_prefix=None,
            preserve_dbpath=False):
        """Initialize MongoDFixture with different options for the mongod process."""

        self.mongod_options = make_historic(utils.default_if_none(mongod_options, {}))
        interface.Fixture.__init__(self, logger, job_num, dbpath_prefix=dbpath_prefix)

        if "dbpath" in self.mongod_options and dbpath_prefix is not None:
            raise ValueError("Cannot specify both mongod_options.dbpath and dbpath_prefix")

        # Default to command line options if the YAML configuration is not passed in.
        self.mongod_executable = utils.default_if_none(mongod_executable, config.MONGOD_EXECUTABLE)

        self.mongod_options = make_historic(utils.default_if_none(mongod_options, {})).copy()

        # The dbpath in mongod_options takes precedence over other settings to make it easier for
        # users to specify a dbpath containing data to test against.
        if "dbpath" not in self.mongod_options:
            self.mongod_options["dbpath"] = os.path.join(self._dbpath_prefix, config.FIXTURE_SUBDIR)
        self._dbpath = self.mongod_options["dbpath"]

        if config.ALWAYS_USE_LOG_FILES:
            self.mongod_options["logpath"] = self._dbpath + "/mongod.log"
            self.mongod_options["logappend"] = ""
            self.preserve_dbpath = True
        else:
            self.preserve_dbpath = preserve_dbpath

        self.mongod = None
        self.port = None

    def setup(self):
        """Set up the mongod."""
        if not self.preserve_dbpath and os.path.lexists(self._dbpath):
            utils.rmtree(self._dbpath, ignore_errors=False)

        try:
            os.makedirs(self._dbpath)
        except os.error:
            # Directory already exists.
            pass

        if "port" not in self.mongod_options:
            self.mongod_options["port"] = core.network.PortAllocator.next_fixture_port(self.job_num)
        self.port = self.mongod_options["port"]

        mongod = _mongod_program(self.logger, self.job_num, executable=self.mongod_executable,
                                 mongod_options=self.mongod_options)
        try:
            self.logger.info("Starting mongod on port %d...\n%s", self.port, mongod.as_command())
            mongod.start()
            self.logger.info("mongod started on port %d with pid %d.", self.port, mongod.pid)
        except Exception as err:
            msg = "Failed to start mongod on port {:d}: {}".format(self.port, err)
            self.logger.exception(msg)
            raise errors.ServerFailure(msg)

        self.mongod = mongod

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = [x.pid for x in [self.mongod] if x is not None]
        if not out:
            self.logger.debug('Mongod not running when gathering standalone fixture pid.')
        return out

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        deadline = time.time() + MongoDFixture.AWAIT_READY_TIMEOUT_SECS

        # Wait until the mongod is accepting connections. The retry logic is necessary to support
        # versions of PyMongo <3.0 that immediately raise a ConnectionFailure if a connection cannot
        # be established.
        while True:
            # Check whether the mongod exited for some reason.
            exit_code = self.mongod.poll()
            if exit_code is not None:
                raise errors.ServerFailure("Could not connect to mongod on port {}, process ended"
                                           " unexpectedly with code {}.".format(
                                               self.port, exit_code))

            try:
                # Use a shorter connection timeout to more closely satisfy the requested deadline.
                client = self.mongo_client(timeout_millis=500)
                client.admin.command("ping")
                break
            except pymongo.errors.ConnectionFailure:
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    raise errors.ServerFailure(
                        "Failed to connect to mongod on port {} after {} seconds".format(
                            self.port, MongoDFixture.AWAIT_READY_TIMEOUT_SECS))

                self.logger.info("Waiting to connect to mongod on port %d.", self.port)
                time.sleep(0.1)  # Wait a little bit before trying again.

        self.logger.info("Successfully contacted the mongod on port %d.", self.port)

    def _do_teardown(self, mode=None):
        if self.mongod is None:
            self.logger.warning("The mongod fixture has not been set up yet.")
            return  # Still a success even if nothing is running.

        if mode == interface.TeardownMode.ABORT:
            self.logger.info(
                "Attempting to send SIGABRT from resmoke to mongod on port %d with pid %d...",
                self.port, self.mongod.pid)
        else:
            self.logger.info("Stopping mongod on port %d with pid %d...", self.port,
                             self.mongod.pid)
        if not self.is_running():
            exit_code = self.mongod.poll()
            msg = ("mongod on port {:d} was expected to be running, but wasn't. "
                   "Process exited with code {:d}.").format(self.port, exit_code)
            self.logger.warning(msg)
            raise errors.ServerFailure(msg)

        self.mongod.stop(mode)
        exit_code = self.mongod.wait()

        # Python's subprocess module returns negative versions of system calls.
        # pylint: disable=invalid-unary-operand-type
        if exit_code == 0 or (mode is not None and exit_code == -(mode.value)):
            self.logger.info("Successfully stopped the mongod on port {:d}.".format(self.port))
        else:
            self.logger.warning("Stopped the mongod on port {:d}. "
                                "Process exited with code {:d}.".format(self.port, exit_code))
            raise errors.ServerFailure(
                "mongod on port {:d} with pid {:d} exited with code {:d}".format(
                    self.port, self.mongod.pid, exit_code))

    def is_running(self):
        """Return true if the mongod is still operating."""
        return self.mongod is not None and self.mongod.poll() is None

    def get_dbpath_prefix(self):
        """Return the _dbpath, as this is the root of the data directory."""
        return self._dbpath

    def get_node_info(self):
        """Return a list of NodeInfo objects."""
        info = interface.NodeInfo(full_name=self.logger.full_name, name=self.logger.name,
                                  port=self.port, pid=self.mongod.pid)
        return [info]

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        if self.mongod is None:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")

        return "localhost:%d" % self.port

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return "mongodb://" + self.get_internal_connection_string()


# Utilities to actually launch the mongod program.


def get_default_log_component_verbosity_for_mongod(executable):
    """Return the correct default 'logComponentVerbosity' value for the executable version."""
    if executable == LAST_LTS_MONGOD_BINARY:
        return default_last_lts_mongod_log_component_verbosity()
    return default_mongod_log_component_verbosity()


def default_mongod_log_component_verbosity():
    """Return the default 'logComponentVerbosity' value to use for mongod processes."""
    if config.EVERGREEN_TASK_ID:
        return DEFAULT_EVERGREEN_MONGOD_LOG_COMPONENT_VERBOSITY
    return DEFAULT_MONGOD_LOG_COMPONENT_VERBOSITY


def default_last_lts_mongod_log_component_verbosity():
    """Return the default 'logComponentVerbosity' value to use for last-lts mongod processes."""
    if config.EVERGREEN_TASK_ID:
        return DEFAULT_EVERGREEN_LAST_LTS_MONGOD_LOG_COMPONENT_VERBOSITY
    return DEFAULT_LAST_LTS_MONGOD_LOG_COMPONENT_VERBOSITY


def _mongod_program(  # pylint: disable=too-many-branches,too-many-statements
        logger, job_num, executable=None, process_kwargs=None, mongod_options=None):
    """
    Return a Process instance that starts mongod arguments constructed from 'mongod_options'.

    @param logger - The logger to pass into the process.
    @param executable - The mongod executable to run.
    @param process_kwargs - A dict of key-value pairs to pass to the process.
    @param mongod_options - A HistoryDict describing the various options to pass to the mongod.
    """
    executable = utils.default_if_none(executable, config.DEFAULT_MONGOD_EXECUTABLE)
    mongod_options = utils.default_if_none(mongod_options, make_historic({})).copy()

    # Apply the --setParameter command line argument. Command line options to resmoke.py override
    # the YAML configuration.
    # We leave the parameters attached for now so the top-level dict tracks its history.
    suite_set_parameters = mongod_options.setdefault("set_parameters", make_historic({}))

    if config.MONGOD_SET_PARAMETERS is not None:
        suite_set_parameters.update(make_historic(utils.load_yaml(config.MONGOD_SET_PARAMETERS)))

    # Set default log verbosity levels if none were specified.
    if "logComponentVerbosity" not in suite_set_parameters:
        suite_set_parameters[
            "logComponentVerbosity"] = get_default_log_component_verbosity_for_mongod(executable)

    # minNumChunksForSessionsCollection controls the minimum number of chunks the balancer will
    # enforce for the sessions collection. If the actual number of chunks is less, the balancer will
    # issue split commands to create more chunks. As a result, the balancer will also end up moving
    # chunks for the sessions collection to balance the chunks across shards. Unless the suite is
    # explicitly prepared to handle these background migrations, set the parameter to 1.
    if "configsvr" in mongod_options and "minNumChunksForSessionsCollection" not in suite_set_parameters:
        suite_set_parameters["minNumChunksForSessionsCollection"] = 1

    # orphanCleanupDelaySecs controls an artificial delay before cleaning up an orphaned chunk
    # that has migrated off of a shard, meant to allow most dependent queries on secondaries to
    # complete first. It defaults to 900, or 15 minutes, which is prohibitively long for tests.
    # Setting it in the .yml file overrides this.
    if "shardsvr" in mongod_options and "orphanCleanupDelaySecs" not in suite_set_parameters:
        suite_set_parameters["orphanCleanupDelaySecs"] = 1

    # The LogicalSessionCache does automatic background refreshes in the server. This is
    # race-y for tests, since tests trigger their own immediate refreshes instead. Turn off
    # background refreshing for tests. Set in the .yml file to override this.
    if "disableLogicalSessionCacheRefresh" not in suite_set_parameters:
        suite_set_parameters["disableLogicalSessionCacheRefresh"] = True

    # Set coordinateCommitReturnImmediatelyAfterPersistingDecision to false so that tests do
    # not need to rely on causal consistency or explicitly wait for the transaction to finish
    # committing. If we are running LAST_LTS mongoD and the test suite has explicitly set the
    # coordinateCommitReturnImmediatelyAfterPersistingDecision parameter, we remove it from
    # the setParameter list, since coordinateCommitReturnImmediatelyAfterPersistingDecision
    # does not exist prior to 4.7.
    # TODO(SERVER-51682): remove the 'elif' clause on master when 5.0 becomes LAST_LTS.
    if executable != LAST_LTS_MONGOD_BINARY and \
        "coordinateCommitReturnImmediatelyAfterPersistingDecision" not in suite_set_parameters:
        suite_set_parameters["coordinateCommitReturnImmediatelyAfterPersistingDecision"] = False
    elif executable == LAST_LTS_MONGOD_BINARY and \
        "coordinateCommitReturnImmediatelyAfterPersistingDecision" in suite_set_parameters:
        del suite_set_parameters["coordinateCommitReturnImmediatelyAfterPersistingDecision"]

    # TODO SERVER-54593 to remove the special-case handling when 5.0 becomes LAST_LTS.
    if "reshardingMinimumOperationDurationMillis" in suite_set_parameters:
        if executable == LAST_LTS_MONGOD_BINARY:
            del suite_set_parameters["reshardingMinimumOperationDurationMillis"]
    elif executable != LAST_LTS_MONGOD_BINARY:
        suite_set_parameters["reshardingMinimumOperationDurationMillis"] = 5000

    # There's a periodic background thread that checks for and aborts expired transactions.
    # "transactionLifetimeLimitSeconds" specifies for how long a transaction can run before expiring
    # and being aborted by the background thread. It defaults to 60 seconds, which is too short to
    # be reliable for our tests. Setting it to 24 hours, so that it is longer than the Evergreen
    # execution timeout.
    if "transactionLifetimeLimitSeconds" not in suite_set_parameters:
        suite_set_parameters["transactionLifetimeLimitSeconds"] = 24 * 60 * 60

    # Hybrid index builds drain writes received during the build process in batches of 1000 writes
    # by default. Not all tests perform enough writes to exercise the code path where multiple
    # batches are applied, which means certain bugs are harder to encounter. Set this level lower
    # so there are more opportunities to drain writes in multiple batches.
    if "maxIndexBuildDrainBatchSize" not in suite_set_parameters:
        suite_set_parameters["maxIndexBuildDrainBatchSize"] = 10

    # The periodic no-op writer writes an oplog entry of type='n' once every 10 seconds. This has
    # the potential to mask issues such as SERVER-31609 because it allows the operationTime of
    # cluster to advance even if the client is blocked for other reasons. We should disable the
    # periodic no-op writer. Set in the .yml file to override this.
    if "replSet" in mongod_options and "writePeriodicNoops" not in suite_set_parameters:
        suite_set_parameters["writePeriodicNoops"] = False

    # The default time for stepdown and quiesce mode in response to SIGTERM is 15 seconds. Reduce
    # this to 100ms for faster shutdown. On branches 4.4 and earlier, there is no quiesce mode, but
    # the default time for stepdown is 10 seconds.
    # TODO(SERVER-47797): Remove reference to waitForStepDownOnNonCommandShutdown.
    if ("replSet" in mongod_options
            and "waitForStepDownOnNonCommandShutdown" not in suite_set_parameters
            and "shutdownTimeoutMillisForSignaledShutdown" not in suite_set_parameters):
        if executable == LAST_LTS_MONGOD_BINARY:
            suite_set_parameters["waitForStepDownOnNonCommandShutdown"] = False
        else:
            suite_set_parameters["shutdownTimeoutMillisForSignaledShutdown"] = 100

    if "enableFlowControl" not in suite_set_parameters and config.FLOW_CONTROL is not None:
        suite_set_parameters["enableFlowControl"] = (config.FLOW_CONTROL == "on")

    if ("failpoint.flowControlTicketOverride" not in suite_set_parameters
            and config.FLOW_CONTROL_TICKETS is not None):
        suite_set_parameters["failpoint.flowControlTicketOverride"] = make_historic(
            {"mode": "alwaysOn", "data": {"numTickets": config.FLOW_CONTROL_TICKETS}})

    add_testing_set_parameters(suite_set_parameters)

    shortcut_opts = {
        "enableMajorityReadConcern": config.MAJORITY_READ_CONCERN,
        "nojournal": config.NO_JOURNAL,
        "storageEngine": config.STORAGE_ENGINE,
        "transportLayer": config.TRANSPORT_LAYER,
        "wiredTigerCollectionConfigString": config.WT_COLL_CONFIG,
        "wiredTigerEngineConfigString": config.WT_ENGINE_CONFIG,
        "wiredTigerIndexConfigString": config.WT_INDEX_CONFIG,
    }

    if config.STORAGE_ENGINE == "inMemory":
        shortcut_opts["inMemorySizeGB"] = config.STORAGE_ENGINE_CACHE_SIZE
    elif config.STORAGE_ENGINE == "rocksdb":
        shortcut_opts["rocksdbCacheSizeGB"] = config.STORAGE_ENGINE_CACHE_SIZE
    elif config.STORAGE_ENGINE == "wiredTiger" or config.STORAGE_ENGINE is None:
        shortcut_opts["wiredTigerCacheSizeGB"] = config.STORAGE_ENGINE_CACHE_SIZE

    # These options are just flags, so they should not take a value.
    opts_without_vals = ("nojournal", "logappend")

    # Have the --nojournal command line argument to resmoke.py unset the journal option.
    if shortcut_opts["nojournal"] and "journal" in mongod_options:
        del mongod_options["journal"]

    # Ensure that config servers run with journaling enabled.
    if "configsvr" in mongod_options:
        shortcut_opts["nojournal"] = False
        mongod_options["journal"] = ""

    # Command line options override the YAML configuration.
    for opt_name in shortcut_opts:
        opt_value = shortcut_opts[opt_name]
        if opt_name in opts_without_vals:
            # Options that are specified as --flag on the command line are represented by a boolean
            # value where True indicates that the flag should be included in 'kwargs'.
            if opt_value:
                mongod_options[opt_name] = ""
        else:
            # Options that are specified as --key=value on the command line are represented by a
            # value where None indicates that the key-value pair shouldn't be included in 'kwargs'.
            if opt_value is not None:
                mongod_options[opt_name] = opt_value

    # Override the storage engine specified on the command line with "wiredTiger" if running a
    # config server replica set.
    if "replSet" in mongod_options and "configsvr" in mongod_options:
        mongod_options["storageEngine"] = "wiredTiger"

    return core.programs.mongod_program(logger, job_num, executable, process_kwargs, mongod_options)


def add_testing_set_parameters(suite_set_parameters):
    """
    Add certain behaviors should only be enabled for resmoke usage.

    These are traditionally enable new commands, insecure access, and increased diagnostics.
    """
    suite_set_parameters.setdefault("testingDiagnosticsEnabled", True)
    suite_set_parameters.setdefault("enableTestCommands", True)
