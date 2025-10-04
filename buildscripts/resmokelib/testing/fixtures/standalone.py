"""Standalone mongod fixture for executing JSTests against."""

import os
import os.path
import shutil
import time
import uuid
from typing import Optional

import pymongo
import pymongo.errors
import yaml

from buildscripts.resmokelib import logging
from buildscripts.resmokelib.extensions import (
    delete_extension_configs,
    find_and_generate_extension_configs,
)
from buildscripts.resmokelib.testing.fixtures import interface
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib
from buildscripts.resmokelib.utils.history import HistoryDict


class MongoDFixture(interface.Fixture, interface._DockerComposeInterface):
    """Fixture which provides JSTests with a standalone mongod to run against."""

    def __init__(
        self,
        logger: logging.Logger,
        job_num: int,
        fixturelib: FixtureLib,
        mongod_executable: Optional[str] = None,
        mongod_options: Optional[dict] = None,
        add_feature_flags: bool = False,
        dbpath_prefix: Optional[str] = None,
        preserve_dbpath: bool = False,
        port: Optional[int] = None,
        launch_mongot: bool = False,
        load_all_extensions: bool = False,
    ):
        """Initialize MongoDFixture with different options for the mongod process.

        Args:
            logger (logging.Logger): logger
            job_num (int): Which job this fixture is a part of. Used for multithreading
            fixturelib (FixtureLib): fixturelib
            mongod_executable (Optional[str], optional): Optional path to mongod executable. Defaults to None.
            mongod_options (Optional[dict], optional): Optional mongod startup options. Defaults to None.
            add_feature_flags (bool, optional): Sets all feature flags to true when set. Defaults to False.
            dbpath_prefix (Optional[str], optional): Sets the dbpath_prefix. Defaults to None.
            preserve_dbpath (bool, optional): preserve_dbpath. Defaults to False.
            port (Optional[int], optional): Port to use for mongod. Defaults to None.
            launch_mongot (bool, optional): Should mongot be launched as well. Defaults to False.
            load_all_extensions (bool, optional): Whether to load all test extensions upon startup. Defaults to False.

        Raises
            ValueError: _description_
        """
        interface.Fixture.__init__(self, logger, job_num, fixturelib, dbpath_prefix=dbpath_prefix)
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

        if "set_parameters" not in self.mongod_options:
            self.mongod_options["set_parameters"] = {}

        if add_feature_flags:
            for ff in self.config.ENABLED_FEATURE_FLAGS:
                self.mongod_options["set_parameters"][ff] = "true"

        if "dbpath" in self.mongod_options and dbpath_prefix is not None:
            raise ValueError("Cannot specify both mongod_options.dbpath and dbpath_prefix")

        # Default to command line options if the YAML configuration is not passed in.
        self.mongod_executable = self.fixturelib.default_if_none(
            mongod_executable, self.config.MONGOD_EXECUTABLE
        )

        # The dbpath in mongod_options takes precedence over other settings to make it easier for
        # users to specify a dbpath containing data to test against.
        if "dbpath" not in self.mongod_options:
            self.mongod_options["dbpath"] = os.path.join(
                self._dbpath_prefix, self.config.FIXTURE_SUBDIR
            )
        self._dbpath = self.mongod_options["dbpath"]

        if self.config.ALWAYS_USE_LOG_FILES:
            self.mongod_options["logpath"] = self._dbpath + "/mongod.log"
            self.mongod_options["logappend"] = ""
            self.preserve_dbpath = True
        else:
            self.preserve_dbpath = preserve_dbpath

        self.mongod = None
        self.port = port or fixturelib.get_next_port(job_num)
        self.mongod_options["port"] = self.port

        if launch_mongot:
            self.launch_mongot_bool = True

            # mongot exposes two ports that it will listen for ingress communication on: "port",
            # which expects the MongoRPC protocol, and "grpcPort", which expects the MongoDB
            # gRPC protocol. When useGrpcForSearch is true, mongos and mongod will communicate
            # with mongot using gRPC, and so we must set the "mongotHost" option to the listening
            # address that expects the gRPC protocol. However, the testing infrastructure also
            # communicates with mongot directly using the pymongo driver, which must communicate
            # using MongoRPC, and so we also setup the "port" on mongot to listen for MongoRPC
            # connections no matter what.
            self.mongot_port = fixturelib.get_next_port(job_num)
            if self.mongod_options["set_parameters"].get("useGrpcForSearch"):
                self.mongot_grpc_port = fixturelib.get_next_port(job_num)
                self.mongod_options["mongotHost"] = "localhost:" + str(self.mongot_grpc_port)
            else:
                self.mongod_options["mongotHost"] = "localhost:" + str(self.mongot_port)

            # In future architectures, this could change
            self.mongod_options["searchIndexManagementHostAndPort"] = self.mongod_options[
                "mongotHost"
            ]
        else:
            self.launch_mongot_bool = False
        # If a suite enables launching mongot, the necessary startup options for the MongoTFixture will be created in
        # setup_mongot_params() which is called by the builders after all other fixture types have been setup (and
        # therefore all other nodes have been assigned ports, which allows mongot to connect to a given mongod or
        # mongos. The MongoTFixture is then launched by the MongoDFixture in setup().
        self.mongot = None

        if "featureFlagGRPC" in self.config.ENABLED_FEATURE_FLAGS or self.mongod_options[
            "set_parameters"
        ].get("featureFlagGRPC"):
            self.grpcPort = fixturelib.get_next_port(job_num)
            self.mongod_options["grpcPort"] = self.grpcPort

        # Always log backtraces to a file in the dbpath in our testing.
        backtrace_log_file_name = os.path.join(
            self.get_dbpath_prefix(), uuid.uuid4().hex + ".stacktrace"
        )
        self.mongod_options["set_parameters"]["backtraceLogFile"] = backtrace_log_file_name

    def launch_mongot(self):
        mongot = self.fixturelib.make_fixture(
            "MongoTFixture", self.logger, self.job_num, mongot_options=self.mongot_options
        )

        mongot.setup()
        self.mongot = mongot
        self.mongot.await_ready()

    def setup(self, temporary_flags={}):
        """Set up the mongod."""
        if not self.preserve_dbpath and os.path.lexists(self._dbpath):
            shutil.rmtree(self._dbpath, ignore_errors=False)

        os.makedirs(self._dbpath, exist_ok=True)

        launcher = MongodLauncher(self.fixturelib)

        mongod_options = self.mongod_options.copy()
        mongod_options.update(temporary_flags)

        # Second return val is the port, which we ignore because we explicitly created the port above.
        # The port is used to set other mongod_option's here:
        # https://github.com/mongodb/mongo/blob/532a6a8ae7b8e7ab5939e900759c00794862963d/buildscripts/resmokelib/testing/fixtures/replicaset.py#L136
        mongod, _ = launcher.launch_mongod_program(
            self.logger,
            self.job_num,
            executable=self.mongod_executable,
            mongod_options=mongod_options,
        )

        try:
            msg = f"Starting mongod on port { self.port }...\n{ mongod.as_command() }"
            self.logger.info(msg)
            mongod.start()
            msg = f"mongod started on port { self.port } with pid { mongod.pid }"
            self.logger.info(msg)
        except Exception as err:
            msg = "Failed to start mongod on port {:d}: {}".format(self.port, err)
            self.logger.exception(msg)
            raise self.fixturelib.ServerFailure(msg)

        self.mongod = mongod
        if self.launch_mongot_bool:
            self.launch_mongot()

    def _all_mongo_d_s_t(self):
        """Return the standalone `mongod` `Process` instance."""
        return [self]

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = [x.pid for x in [self.mongod] if x is not None]
        if not out:
            self.logger.debug("Mongod not running when gathering standalone fixture pid.")
        return out

    def get_mongod_options(self):
        return self.mongod_options

    def set_mongod_options(self, options):
        self.mongod_options = options

    def _handle_await_ready_retry(self, deadline):
        remaining = deadline - time.time()
        if remaining <= 0.0:
            raise self.fixturelib.ServerFailure(
                "Failed to connect to mongod on port {} after {} seconds".format(
                    self.port, MongoDFixture.AWAIT_READY_TIMEOUT_SECS
                )
            )

        self.logger.info("Waiting to connect to mongod on port %d.", self.port)
        time.sleep(0.1)  # Wait a little bit before trying again.

    def setup_mongot_params(self, router_endpoint_for_mongot: Optional[int] = None):
        mongot_options = {}

        ## Set up mongot's ingress communication for query & index management commands from mongod ##
        # Set up the listening port on mongot expecting the MongoRPC protocol. This is used for
        # direct communication from drivers to mongot, and in the Atlas architecture.
        mongot_options["port"] = self.mongot_port
        # Set up the listening port on mongot expecting the MongoDB gRPC protocol, which will
        # be used when `useGrpcForSearch` is true on mongos/mongod. This is used in the community
        # architecture.
        if self.mongod_options["set_parameters"].get("useGrpcForSearch"):
            mongot_options["grpcPort"] = self.mongot_grpc_port

        ## Set up mongot's egress communication for change stream/replication commands to mongot ###
        # Point the mongodHostAndPort and mongosHostAndPort parameters on mongot to the ingress
        # listening ports of mongod/mongos.
        mongot_options["mongodHostAndPort"] = "localhost:" + str(self.port)
        if router_endpoint_for_mongot is not None:
            mongot_options["mongosHostAndPort"] = "localhost:" + str(router_endpoint_for_mongot)

        if "keyFile" not in self.mongod_options:
            raise self.fixturelib.ServerFailure("Cannot launch mongot without providing a keyfile")

        mongot_options["keyFile"] = self.mongod_options["keyFile"]
        self.mongot_options = mongot_options

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
                raise self.fixturelib.ServerFailure(
                    "Could not connect to mongod on port {}, process ended"
                    " unexpectedly with code {}.".format(self.port, exit_code)
                )

            try:
                # Use a shorter connection timeout to more closely satisfy the requested deadline.
                client = self.mongo_client(timeout_millis=500)
                client.admin.command("ping")
                break
            except pymongo.errors.OperationFailure as err:
                if err.code != self._INTERRUPTED_DUE_TO_STORAGE_CHANGE:
                    raise err
                self._handle_await_ready_retry(deadline)
            except pymongo.errors.ConnectionFailure:
                self._handle_await_ready_retry(deadline)

        self.logger.info("Successfully contacted the mongod on port %d.", self.port)

    def _do_teardown(self, finished=False, mode=None):
        if finished and self.load_all_extensions and self.loaded_extensions:
            delete_extension_configs(self.loaded_extensions, self.logger)

        if self.config.NOOP_MONGO_D_S_PROCESSES:
            self.logger.info(
                "This is running against an External System Under Test setup with `docker-compose.yml` -- skipping teardown."
            )
            return

        if self.mongod is None:
            self.logger.warning("The mongod fixture has not been set up yet.")
            return  # Still a success even if nothing is running.

        if mode == interface.TeardownMode.ABORT:
            self.logger.info(
                "Attempting to send SIGABRT from resmoke to mongod on port %d with pid %d...",
                self.port,
                self.mongod.pid,
            )
        else:
            self.logger.info(
                "Stopping mongod on port %d with pid %d...", self.port, self.mongod.pid
            )
        if not self.is_running():
            exit_code = self.mongod.poll()
            msg = (
                "mongod on port {:d} was expected to be running, but wasn't. "
                "Process exited with code {:d}."
            ).format(self.port, exit_code)
            self.logger.warning(msg)
            raise self.fixturelib.ServerFailure(msg)

        if self.mongot is not None:
            self.mongot._do_teardown(mode)

        self.mongod.stop(mode)
        exit_code = self.mongod.wait()

        # Python's subprocess module returns negative versions of system calls.
        if exit_code == 0 or (mode is not None and exit_code == -(mode.value)):
            self.logger.info("Successfully stopped the mongod on port {:d}.".format(self.port))
        else:
            self.logger.warning(
                "Stopped the mongod on port {:d}. " "Process exited with code {:d}.".format(
                    self.port, exit_code
                )
            )
            raise self.fixturelib.ServerFailure(
                "mongod on port {:d} with pid {:d} exited with code {:d}".format(
                    self.port, self.mongod.pid, exit_code
                )
            )

    def is_running(self):
        """Return true if the mongod is still operating."""
        return self.mongod is not None and self.mongod.poll() is None

    def get_dbpath_prefix(self):
        """Return the _dbpath, as this is the root of the data directory."""
        return self._dbpath

    def get_node_info(self):
        """Return a list of NodeInfo objects."""
        if self.mongod is None:
            self.logger.warning("The mongod fixture has not been set up yet.")
            return []

        info = interface.NodeInfo(
            full_name=self.logger.full_name,
            name=self.logger.name,
            port=self.port,
            pid=self.mongod.pid,
        )
        return [info]

    def _get_hostname(self):
        return (
            self.logger.external_sut_hostname
            if self.config.NOOP_MONGO_D_S_PROCESSES
            else "localhost"
        )

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        return f"{self._get_hostname()}:{self.port}"

    def get_shell_connection_string(self, use_grpc=False):
        port = self.port if not (self.config.SHELL_GRPC or use_grpc) else self.grpcPort
        return f"{self._get_hostname()}:{port}"

    def get_shell_connection_url(self):
        return "mongodb://" + self.get_shell_connection_string()

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return "mongodb://" + self.get_internal_connection_string() + "/?directConnection=true"


# The below parameters define the default 'logComponentVerbosity' object passed to mongod processes
# started either directly via resmoke or those that will get started by the mongo shell. We allow
# this default to be different for tests run locally and tests run in Evergreen. This allows us, for
# example, to keep log verbosity high in Evergreen test runs without polluting the logs for
# developers running local tests.

# The default verbosity setting for any tests that are not started with an Evergreen task id. This
# will apply to any tests run locally.
DEFAULT_MONGOD_LOG_COMPONENT_VERBOSITY = {
    "replication": {"rollback": 2},
    "sharding": {"migration": 2, "rangeDeleter": 2},
    "transaction": 4,
}

# The default verbosity setting for any mongod processes running in Evergreen i.e. started with an
# Evergreen task id.
DEFAULT_EVERGREEN_MONGOD_LOG_COMPONENT_VERBOSITY = {
    "replication": {"election": 4, "heartbeats": 2, "initialSync": 2, "rollback": 2},
    "sharding": {"migration": 2, "rangeDeleter": 2},
    "storage": {"recovery": 2},
    "transaction": 4,
}


class MongodLauncher(object):
    """Class with utilities for launching a mongod."""

    def __init__(self, fixturelib: FixtureLib):
        """Initialize MongodLauncher."""
        self.fixturelib = fixturelib
        self.config = fixturelib.get_config()

    def launch_mongod_program(
        self,
        logger: logging.Logger,
        job_num: str,
        executable: Optional[str] = None,
        process_kwargs: Optional[dict] = None,
        mongod_options: Optional[HistoryDict] = None,
    ) -> tuple["process.Process", HistoryDict]:
        """
        Return a Process instance that starts mongod arguments constructed from 'mongod_options'.

        @param logger - The logger to pass into the process.
        @param executable - The mongod executable to run.
        @param process_kwargs - A dict of key-value pairs to pass to the process.
        @param mongod_options - A HistoryDict describing the various options to pass to the mongod.
        """
        executable = self.fixturelib.default_if_none(
            executable, self.config.DEFAULT_MONGOD_EXECUTABLE
        )
        mongod_options = self.fixturelib.default_if_none(mongod_options, {}).copy()

        # Apply the --setParameter command line argument. Command line options to resmoke.py override
        # the YAML configuration.
        # We leave the parameters attached for now so the top-level dict tracks its history.
        suite_set_parameters = mongod_options.setdefault("set_parameters", {})

        if self.config.MONGOD_SET_PARAMETERS is not None:
            suite_set_parameters.update(yaml.safe_load(self.config.MONGOD_SET_PARAMETERS))

        if "mongotHost" in mongod_options:
            suite_set_parameters["mongotHost"] = mongod_options.pop("mongotHost")
            suite_set_parameters["searchIndexManagementHostAndPort"] = mongod_options.pop(
                "searchIndexManagementHostAndPort"
            )
        # Some storage options are both a mongod option (as in config file option and its equivalent
        # "--xyz" command line parameter) and a "--setParameter". In case of conflict, for instance
        # due to the config fuzzer adding "xyz" as a "--setParameter" when the "--xyz" option is
        # already defined in the suite's YAML, the "--setParameter" value shall be preserved and the
        # "--xyz" option discarded to avoid hitting an error due to conflicting definitions.
        mongod_option_and_set_parameter_conflicts = ["syncdelay", "journalCommitInterval"]
        for key in mongod_option_and_set_parameter_conflicts:
            if key in mongod_options and key in suite_set_parameters:
                del mongod_options[key]

        # Set default log verbosity levels if none were specified.
        if "logComponentVerbosity" not in suite_set_parameters:
            suite_set_parameters["logComponentVerbosity"] = (
                self.get_default_log_component_verbosity_for_mongod()
            )

        # orphanCleanupDelaySecs controls an artificial delay before cleaning up an orphaned chunk
        # that has migrated off of a shard, meant to allow most dependent queries on secondaries to
        # complete first. It defaults to 900, or 15 minutes, which is prohibitively long for tests.
        # Setting it in the .yml file overrides this.
        if "orphanCleanupDelaySecs" not in suite_set_parameters:
            suite_set_parameters["orphanCleanupDelaySecs"] = 1

        # Increase the default config server command timeout to 5 minutes to avoid spurious
        # failures on slow machines.
        if "defaultConfigCommandTimeoutMS" not in suite_set_parameters:
            suite_set_parameters["defaultConfigCommandTimeoutMS"] = 5 * 60 * 1000

        # receiveChunkWaitForRangeDeleterTimeoutMS controls the amount of time an incoming migration
        # will wait for an intersecting range with data in it to be cleared up before failing. The
        # default is 10 seconds, but in some slower variants this is not enough time for the range
        # deleter to finish so we increase it here to 90 seconds. Setting a value for this parameter
        # in the .yml file overrides this.
        if "receiveChunkWaitForRangeDeleterTimeoutMS" not in suite_set_parameters:
            suite_set_parameters["receiveChunkWaitForRangeDeleterTimeoutMS"] = 90000

        # The LogicalSessionCache does automatic background refreshes in the server. This is
        # race-y for tests, since tests trigger their own immediate refreshes instead. Turn off
        # background refreshing for tests. Set in the .yml file to override this.
        if "disableLogicalSessionCacheRefresh" not in suite_set_parameters:
            suite_set_parameters["disableLogicalSessionCacheRefresh"] = True

        # Set coordinateCommitReturnImmediatelyAfterPersistingDecision to false so that tests do
        # not need to rely on causal consistency or explicitly wait for the transaction to finish
        # committing.
        if "coordinateCommitReturnImmediatelyAfterPersistingDecision" not in suite_set_parameters:
            suite_set_parameters["coordinateCommitReturnImmediatelyAfterPersistingDecision"] = False

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
        if (
            "replSet" in mongod_options or "serverless" in mongod_options
        ) and "shutdownTimeoutMillisForSignaledShutdown" not in suite_set_parameters:
            suite_set_parameters["shutdownTimeoutMillisForSignaledShutdown"] = 100

        _add_testing_set_parameters(suite_set_parameters)

        shortcut_opts = {
            "storageEngine": self.config.STORAGE_ENGINE,
            "wiredTigerCollectionConfigString": self.config.WT_COLL_CONFIG,
            "wiredTigerEngineConfigString": self.config.WT_ENGINE_CONFIG,
            "wiredTigerIndexConfigString": self.config.WT_INDEX_CONFIG,
        }
        shortcut_opts.update(self.config.MONGOD_EXTRA_CONFIG)

        if self.config.STORAGE_ENGINE == "inMemory":
            shortcut_opts["inMemorySizeGB"] = self.config.STORAGE_ENGINE_CACHE_SIZE
        elif self.config.STORAGE_ENGINE == "wiredTiger" or self.config.STORAGE_ENGINE is None:
            shortcut_opts["wiredTigerCacheSizeGB"] = self.config.STORAGE_ENGINE_CACHE_SIZE
            shortcut_opts["wiredTigerCacheSizePct"] = self.config.STORAGE_ENGINE_CACHE_SIZE_PCT

        # If a JS_GC_ZEAL value has been provided in the configuration under MOZJS_JS_GC_ZEAL,
        # we inject this value directly as an environment variable to be passed to the spawned
        # mongod process.
        if self.config.MOZJS_JS_GC_ZEAL:
            process_kwargs = self.fixturelib.default_if_none(process_kwargs, {}).copy()
            env_vars = process_kwargs.setdefault("env_vars", {}).copy()
            env_vars.setdefault("JS_GC_ZEAL", self.config.MOZJS_JS_GC_ZEAL)
            process_kwargs["env_vars"] = env_vars

        # These options are just flags, so they should not take a value.
        allowed_opts_without_vals = ["logappend", "directoryperdb", "wiredTigerDirectoryForIndexes"]

        # Ensure that config servers run with journaling enabled.
        # TODO SERVER-97078: Ensure this works for auto bootstrapped config servers
        if "configsvr" in mongod_options:
            suite_set_parameters.setdefault("reshardingMinimumOperationDurationMillis", 5000)
            suite_set_parameters.setdefault(
                "reshardingCriticalSectionTimeoutMillis", 24 * 60 * 60 * 1000
            )  # 24 hours
            suite_set_parameters.setdefault(
                "reshardingDelayBeforeRemainingOperationTimeQueryMillis", 0
            )

        # Command line options override the YAML configuration.
        for opt_name in shortcut_opts:
            opt_value = shortcut_opts[opt_name]
            if opt_name in allowed_opts_without_vals:
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
        if "configsvr" in mongod_options:
            mongod_options["storageEngine"] = "wiredTiger"

        if self.config.CONFIG_FUZZER_ENCRYPTION_OPTS:
            for opt_name in self.config.CONFIG_FUZZER_ENCRYPTION_OPTS:
                if opt_name in mongod_options:
                    continue

                mongod_options[opt_name] = self.config.CONFIG_FUZZER_ENCRYPTION_OPTS[opt_name]

        return self.fixturelib.mongod_program(
            logger, job_num, executable, process_kwargs, mongod_options
        )

    def get_default_log_component_verbosity_for_mongod(self):
        """Return the default 'logComponentVerbosity' value to use for mongod processes."""
        if self.config.EVERGREEN_TASK_ID:
            return DEFAULT_EVERGREEN_MONGOD_LOG_COMPONENT_VERBOSITY
        return DEFAULT_MONGOD_LOG_COMPONENT_VERBOSITY


def _add_testing_set_parameters(suite_set_parameters):
    """
    Add certain behaviors should only be enabled for resmoke usage.

    These are traditionally enable new commands, insecure access, and increased diagnostics.
    """
    suite_set_parameters.setdefault("testingDiagnosticsEnabled", True)
    suite_set_parameters.setdefault("enableTestCommands", True)
    # The exact file location is on a per-process basis, so it'll have to be determined when the process gets spun up.
    # Set it to true for now as a placeholder that will error if no further processing is done.
    # The placeholder is needed so older versions don't have this option won't have this value set.
    suite_set_parameters.setdefault("backtraceLogFile", True)
    suite_set_parameters.setdefault("disableTransitionFromLatestToLastContinuous", False)
    suite_set_parameters.setdefault("oplogApplicationEnforcesSteadyStateConstraints", True)
