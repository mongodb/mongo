"""Mongot fixture for executing JSTests against.

Mongot is a MongoDB-specific process written as a wrapper around Lucene. Using Lucene, mongot indexes MongoDB databases to provide our customers with full text search capabilities.

Customers have the option of running mongot on Atlas or locally using a special "local-dev" binary of mongot. The local-dev binary allows mongot and mongod to speak directly on the localhost, rather than via proprietary network proxies configured by the Atlas Data Plane. 

A resmoke suite's yml definition can enable launching mongot(s) enabled via the launch_mongot option on the ReplicaSetFixture and providing a keyfile. If enabled, the ReplicaSetFixture launches a local-dev version of mongot per mongod node. The mongot replicates directly from the co-located
mongod via a $changeStream. 
"""

import os
import os.path
import time
import shutil
import uuid

import yaml

import pymongo
import pymongo.errors

from buildscripts.resmokelib.testing.fixtures import interface


class MongoTFixture(interface.Fixture, interface._DockerComposeInterface):
    """Fixture which provides JSTests with a mongot to run alongside a mongod."""

    def __init__(self, logger, job_num, fixturelib, dbpath_prefix=None, mongot_options=None):
        interface.Fixture.__init__(self, logger, job_num, fixturelib)
        self.mongot_options = self.fixturelib.make_historic(
            self.fixturelib.default_if_none(mongot_options, {}))
        # Default to command line options if the YAML configuration is not passed in.
        self.mongot_executable = self.fixturelib.default_if_none(self.config.MONGOT_EXECUTABLE)
        self.port = self.mongot_options["port"]
        # Each mongot requires its own unique config journal to persist index definitions, replication status, etc to disk.
        # If dir passed to --data-dir option doesn't exist, mongot will create it
        self.data_dir = "data/config_journal_" + str(self.port)
        self.mongot_options["data-dir"] = self.data_dir
        self.mongot = None

    def setup(self):
        """Set up and launch the mongot."""
        launcher = MongotLauncher(self.fixturelib)
        # Second return val is the port, which we ignore because we explicitly generated the port number in MongoDFixture
        # initialization and save to MongotFixture in above initialization function.
        mongot, _ = launcher.launch_mongot_program(self.logger, self.job_num,
                                                   executable=self.mongot_executable,
                                                   mongot_options=self.mongot_options)

        try:
            msg = f"Starting mongot on port { self.port } ...\n{ mongot.as_command() }"
            self.logger.info(msg)
            mongot.start()
            msg = f"mongot started on port { self.port } with pid { mongot.pid }"
            self.logger.info(msg)
        except Exception as err:
            msg = "Failed to start mongot on port {:d}: {}".format(self.port, err)
            self.logger.exception(msg)
            raise self.fixturelib.ServerFailure(msg)

        self.mongot = mongot

    def _all_mongo_d_s_t(self):
        """Return the `mongot` `Process` instance."""
        return [self]

    def pids(self):
        """:return: pids owned by this fixture if any."""
        out = [x.pid for x in [self.mongot] if x is not None]
        if not out:
            self.logger.debug('Mongot not running when gathering mongot fixture pid.')
        return out

    def _do_teardown(self, mode=None):

        if self.config.NOOP_MONGO_D_S_PROCESSES:
            self.logger.info(
                "This is running against an External System Under Test setup with `docker-compose.yml` -- skipping teardown."
            )
            return

        if self.mongot is None:
            self.logger.warning("The mongot fixture has not been set up yet.")
            return  # Still a success even if nothing is running.

        if mode == interface.TeardownMode.ABORT:
            self.logger.info(
                "Attempting to send SIGABRT from resmoke to mongot on port %d with pid %d...",
                self.port, self.mongot.pid)
        else:
            self.logger.info("Stopping mongot on port %d with pid %d...", self.port,
                             self.mongot.pid)
        if not self.is_running():
            exit_code = self.mongot.poll()
            msg = ("mongot on port {:d} was expected to be running, but wasn't. "
                   "Process exited with code {:d}.").format(self.port, exit_code)
            self.logger.warning(msg)
            raise self.fixturelib.ServerFailure(msg)

        self.mongot.stop(mode)
        exit_code = self.mongot.wait()

        # Java applications return exit code of 143 when they shut down upon receiving and obeying a SIGTERM signal, which is the desired/default mode.
        if exit_code == 143 or (mode is not None and exit_code == -(mode.value)):
            self.logger.info("Successfully stopped the mongot on port {:d}.".format(self.port))
        else:
            self.logger.warning("Stopped the mongot on port {:d}. "
                                "Process exited with code {:d}.".format(self.port, exit_code))
            raise self.fixturelib.ServerFailure(
                "mongot on port {:d} with pid {:d} exited with code {:d}".format(
                    self.port, self.mongot.pid, exit_code))

    def is_running(self):
        """Return true if the mongot is still operating."""
        return self.mongot is not None and self.mongot.poll() is None

    def get_dbpath_prefix(self):
        """Return the _dbpath, as this is the root of the data directory."""
        return self._dbpath

    def get_node_info(self):
        """Return a list of NodeInfo objects."""
        if self.mongot is None:
            self.logger.warning("The mongot fixture has not been set up yet.")
            return []

        info = interface.NodeInfo(full_name=self.logger.full_name, name=self.logger.name,
                                  port=self.port, pid=self.mongot.pid, router_port=self.router_port)
        return [info]

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        return f"localhost:{self.port}"

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return "mongodb://" + self.get_internal_connection_string() + "/?directConnection=true"

    def await_ready(self):
        """Block until the fixture can be used for testing."""
        deadline = time.time() + MongoTFixture.AWAIT_READY_TIMEOUT_SECS

        # Wait until the mongot is accepting connections. The retry logic is necessary to support
        # versions of PyMongo <3.0 that immediately raise a ConnectionFailure if a connection cannot
        # be established.
        while True:
            # Check whether the mongot exited for some reason.
            exit_code = self.mongot.poll()
            if exit_code is not None:
                raise self.fixturelib.ServerFailure(
                    "Could not connect to mongot on port {}, process ended"
                    " unexpectedly with code {}.".format(self.port, exit_code))

            try:
                # By connecting to the host and port that mongot is listening from,
                # we ensure mongot specifically is receiving the ping command.
                client = pymongo.MongoClient(self.get_driver_connection_url())
                client.admin.command("hello")
                break
            except pymongo.errors.ConnectionFailure:
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    raise self.fixturelib.ServerFailure(
                        "Failed to connect to mongot on port {} after {} seconds".format(
                            self.port, MongoTFixture.AWAIT_READY_TIMEOUT_SECS))

                self.logger.info("Waiting to connect to mongot on port %d.", self.port)
                time.sleep(0.1)  # Wait a little bit before trying again.

        self.logger.info("Successfully contacted the mongot on port %d.", self.port)


class MongotLauncher(object):
    """Class with utilities for launching a mongot."""

    def __init__(self, fixturelib):
        """Initialize MongotLauncher."""
        self.fixturelib = fixturelib
        self.config = fixturelib.get_config()

    def launch_mongot_program(self, logger, job_num, executable=None, process_kwargs=None,
                              mongot_options=None):
        """
        Return a Process instance that starts a mongot with arguments constructed from 'mongot_options'.

        @param logger - The logger to pass into the process.
        @param executable - The mongot executable to run.
        @param process_kwargs - A dict of key-value pairs to pass to the process.
        @param mongot_options - A HistoryDict describing the various options to pass to the mongot.
        
        Currently, this will launch a mongot with --port, --mongodHostAndPort, and --keyFile commandline 
        options. To support launching mongot with more startup options, those new options would need to 
        be added to mongot_options in MongoTFixture initialization or, if mongod needs to share/know the 
        mongot startup option (like in the case of keyFile), in MongoDFixture::setup_mongot().
        """

        executable = self.fixturelib.default_if_none(executable,
                                                     self.config.DEFAULT_MONGOD_EXECUTABLE)
        mongot_options = self.fixturelib.default_if_none(mongot_options, {}).copy()

        return self.fixturelib.mongot_program(logger, job_num, executable, process_kwargs,
                                              mongot_options)
