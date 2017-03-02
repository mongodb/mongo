"""
Standalone bongod fixture for executing JSTests against.
"""

from __future__ import absolute_import

import os
import os.path
import shutil
import socket
import time

import pybongo

from . import interface
from ... import config
from ... import core
from ... import errors
from ... import utils


class BongoDFixture(interface.Fixture):
    """
    Fixture which provides JSTests with a standalone bongod to run
    against.
    """

    AWAIT_READY_TIMEOUT_SECS = 300

    def __init__(self,
                 logger,
                 job_num,
                 bongod_executable=None,
                 bongod_options=None,
                 dbpath_prefix=None,
                 preserve_dbpath=False):

        interface.Fixture.__init__(self, logger, job_num)

        if "dbpath" in bongod_options and dbpath_prefix is not None:
            raise ValueError("Cannot specify both bongod_options.dbpath and dbpath_prefix")

        # Command line options override the YAML configuration.
        self.bongod_executable = utils.default_if_none(config.BONGOD_EXECUTABLE, bongod_executable)

        self.bongod_options = utils.default_if_none(bongod_options, {}).copy()
        self.preserve_dbpath = preserve_dbpath

        # The dbpath in bongod_options takes precedence over other settings to make it easier for
        # users to specify a dbpath containing data to test against.
        if "dbpath" not in self.bongod_options:
            # Command line options override the YAML configuration.
            dbpath_prefix = utils.default_if_none(config.DBPATH_PREFIX, dbpath_prefix)
            dbpath_prefix = utils.default_if_none(dbpath_prefix, config.DEFAULT_DBPATH_PREFIX)
            self.bongod_options["dbpath"] = os.path.join(dbpath_prefix,
                                                         "job%d" % (self.job_num),
                                                         config.FIXTURE_SUBDIR)
        self._dbpath = self.bongod_options["dbpath"]

        self.bongod = None

    def setup(self):
        if not self.preserve_dbpath:
            shutil.rmtree(self._dbpath, ignore_errors=True)

        try:
            os.makedirs(self._dbpath)
        except os.error:
            # Directory already exists.
            pass

        if "port" not in self.bongod_options:
            self.bongod_options["port"] = core.network.PortAllocator.next_fixture_port(self.job_num)
        self.port = self.bongod_options["port"]

        bongod = core.programs.bongod_program(self.logger,
                                              executable=self.bongod_executable,
                                              **self.bongod_options)
        try:
            self.logger.info("Starting bongod on port %d...\n%s", self.port, bongod.as_command())
            bongod.start()
            self.logger.info("bongod started on port %d with pid %d.", self.port, bongod.pid)
        except:
            self.logger.exception("Failed to start bongod on port %d.", self.port)
            raise

        self.bongod = bongod

    def await_ready(self):
        deadline = time.time() + BongoDFixture.AWAIT_READY_TIMEOUT_SECS

        # Wait until the bongod is accepting connections. The retry logic is necessary to support
        # versions of PyBongo <3.0 that immediately raise a ConnectionFailure if a connection cannot
        # be established.
        while True:
            # Check whether the bongod exited for some reason.
            exit_code = self.bongod.poll()
            if exit_code is not None:
                raise errors.ServerFailure("Could not connect to bongod on port %d, process ended"
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
                        "Failed to connect to bongod on port %d after %d seconds"
                        % (self.port, BongoDFixture.AWAIT_READY_TIMEOUT_SECS))

                self.logger.info("Waiting to connect to bongod on port %d.", self.port)
                time.sleep(0.1)  # Wait a little bit before trying again.

        self.logger.info("Successfully contacted the bongod on port %d.", self.port)

    def teardown(self):
        running_at_start = self.is_running()
        success = True  # Still a success even if nothing is running.

        if not running_at_start and self.port is not None:
            self.logger.info("bongod on port %d was expected to be running in teardown(), but"
                             " wasn't." % (self.port))

        if self.bongod is not None:
            if running_at_start:
                self.logger.info("Stopping bongod on port %d with pid %d...",
                                 self.port,
                                 self.bongod.pid)
                self.bongod.stop()

            exit_code = self.bongod.wait()
            success = exit_code == 0

            if running_at_start:
                self.logger.info("Successfully terminated the bongod on port %d, exited with code"
                                 " %d.",
                                 self.port,
                                 exit_code)

        return success

    def is_running(self):
        return self.bongod is not None and self.bongod.poll() is None

    def get_connection_string(self):
        if self.bongod is None:
            raise ValueError("Must call setup() before calling get_connection_string()")

        return "%s:%d" % (socket.gethostname(), self.port)
