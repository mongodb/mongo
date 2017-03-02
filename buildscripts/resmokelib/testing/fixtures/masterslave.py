"""
Master/slave fixture for executing JSTests against.
"""

from __future__ import absolute_import

import os.path
import socket

import pybongo

from . import interface
from . import standalone
from ... import config
from ... import logging
from ... import utils


class MasterSlaveFixture(interface.ReplFixture):
    """
    Fixture which provides JSTests with a master/slave deployment to
    run against.
    """

    def __init__(self,
                 logger,
                 job_num,
                 bongod_executable=None,
                 bongod_options=None,
                 master_options=None,
                 slave_options=None,
                 dbpath_prefix=None,
                 preserve_dbpath=False):

        interface.ReplFixture.__init__(self, logger, job_num)

        if "dbpath" in bongod_options:
            raise ValueError("Cannot specify bongod_options.dbpath")

        self.bongod_executable = bongod_executable
        self.bongod_options = utils.default_if_none(bongod_options, {})
        self.master_options = utils.default_if_none(master_options, {})
        self.slave_options = utils.default_if_none(slave_options, {})
        self.preserve_dbpath = preserve_dbpath

        # Command line options override the YAML configuration.
        dbpath_prefix = utils.default_if_none(config.DBPATH_PREFIX, dbpath_prefix)
        dbpath_prefix = utils.default_if_none(dbpath_prefix, config.DEFAULT_DBPATH_PREFIX)
        self._dbpath_prefix = os.path.join(dbpath_prefix,
                                           "job%d" % (self.job_num),
                                           config.FIXTURE_SUBDIR)

        self.master = None
        self.slave = None

    def setup(self):
        if self.master is None:
            self.master = self._new_bongod_master()
        self.master.setup()
        self.port = self.master.port

        if self.slave is None:
            self.slave = self._new_bongod_slave()
        self.slave.setup()

    def await_ready(self):
        self.master.await_ready()
        self.slave.await_ready()

        # Do a replicated write to ensure that the slave has finished with its initial sync before
        # starting to run any tests.
        client = utils.new_bongo_client(self.port)

        # Keep retrying this until it times out waiting for replication.
        def insert_fn(remaining_secs):
            remaining_millis = int(round(remaining_secs * 1000))
            write_concern = pybongo.WriteConcern(w=2, wtimeout=remaining_millis)
            coll = client.resmoke.get_collection("await_ready", write_concern=write_concern)
            coll.insert_one({"awaiting": "ready"})

        try:
            self.retry_until_wtimeout(insert_fn)
        except pybongo.errors.WTimeoutError:
            self.logger.info("Replication of write operation timed out.")
            raise

    def teardown(self):
        running_at_start = self.is_running()
        success = True  # Still a success if nothing is running.

        if not running_at_start:
            self.logger.info("Master-slave deployment was expected to be running in teardown(),"
                             " but wasn't.")

        if self.slave is not None:
            if running_at_start:
                self.logger.info("Stopping slave...")

            success = self.slave.teardown()

            if running_at_start:
                self.logger.info("Successfully stopped slave.")

        if self.master is not None:
            if running_at_start:
                self.logger.info("Stopping master...")

            success = self.master.teardown() and success

            if running_at_start:
                self.logger.info("Successfully stopped master.")

        return success

    def is_running(self):
        return (self.master is not None and self.master.is_running() and
                self.slave is not None and self.slave.is_running())

    def get_primary(self):
        return self.master

    def get_secondaries(self):
        return [self.slave]

    def _new_bongod(self, bongod_logger, bongod_options):
        """
        Returns a standalone.BongoDFixture with the specified logger and
        options.
        """
        return standalone.BongoDFixture(bongod_logger,
                                        self.job_num,
                                        bongod_executable=self.bongod_executable,
                                        bongod_options=bongod_options,
                                        preserve_dbpath=self.preserve_dbpath)

    def _new_bongod_master(self):
        """
        Returns a standalone.BongoDFixture configured to be used as the
        master of a master-slave deployment.
        """

        logger_name = "%s:master" % (self.logger.name)
        bongod_logger = logging.loggers.new_logger(logger_name, parent=self.logger)

        bongod_options = self.bongod_options.copy()
        bongod_options.update(self.master_options)
        bongod_options["master"] = ""
        bongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "master")
        return self._new_bongod(bongod_logger, bongod_options)

    def _new_bongod_slave(self):
        """
        Returns a standalone.BongoDFixture configured to be used as the
        slave of a master-slave deployment.
        """

        logger_name = "%s:slave" % (self.logger.name)
        bongod_logger = logging.loggers.new_logger(logger_name, parent=self.logger)

        bongod_options = self.bongod_options.copy()
        bongod_options.update(self.slave_options)
        bongod_options["slave"] = ""
        bongod_options["source"] = "%s:%d" % (socket.gethostname(), self.port)
        bongod_options["dbpath"] = os.path.join(self._dbpath_prefix, "slave")
        return self._new_bongod(bongod_logger, bongod_options)
