"""Standalone mongod fixture for executing JSTests against."""

import os
import os.path
import time

import pymongo
import pymongo.errors

from . import interface
from ... import config
from ... import core
from ... import errors
from ... import utils


class MongoDFixture(interface.Fixture):
    """Fixture which provides JSTests with a standalone mongod to run against."""

    AWAIT_READY_TIMEOUT_SECS = 300

    def __init__(  # pylint: disable=too-many-arguments
            self, logger, job_num, mongod_executable=None, mongod_options=None, dbpath_prefix=None,
            preserve_dbpath=False):
        """Initialize MongoDFixture with different options for the mongod process."""

        self.mongod_options = utils.default_if_none(mongod_options, {})
        interface.Fixture.__init__(self, logger, job_num, dbpath_prefix=dbpath_prefix)

        if "dbpath" in self.mongod_options and dbpath_prefix is not None:
            raise ValueError("Cannot specify both mongod_options.dbpath and dbpath_prefix")

        # Default to command line options if the YAML configuration is not passed in.
        self.mongod_executable = utils.default_if_none(mongod_executable, config.MONGOD_EXECUTABLE)

        self.mongod_options = utils.default_if_none(mongod_options, {}).copy()

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

        mongod = core.programs.mongod_program(self.logger, executable=self.mongod_executable,
                                              **self.mongod_options)
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

    def get_internal_connection_string(self):
        """Return the internal connection string."""
        if self.mongod is None:
            raise ValueError("Must call setup() before calling get_internal_connection_string()")

        return "localhost:%d" % self.port

    def get_driver_connection_url(self):
        """Return the driver connection URL."""
        return "mongodb://" + self.get_internal_connection_string()
