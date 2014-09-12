"""
Fixtures for the execution of JSTests
"""

import os
import shutil
import time

from external_programs import *
from mongodb_programs import MongoD
from mongodb_programs import MONGOD_DEFAULT_DATA_PATH
from mongodb_programs import MONGOD_DEFAULT_EXEC

DEFAULT_FIXTURE_CLASSES = {"mongodb_server": "smoke.SingleMongoDFixture",
                           "shell_globals": "smoke.GlobalShellFixture"}


class Fixture(object):

    """Base class for all fixture objects - require suite setup and teardown and api per-test."""

    def __init__(self, logger):
        self.logger = logger

    def setup(self):
        pass

    def build_api(self, test_type, test_logger):
        pass

    def teardown(self):
        pass


class SimpleFixture(Fixture):

    """Simple fixture objects do not have extra state per-test.

    This means they can implement the api by just implementing the add_to_<type> methods.

    Fixtures which need to do per-test logging cannot use this simplification, for example.
    """

    def __init__(self, logger):
        Fixture.__init__(self, logger)

    def build_api(self, test_type, test_logger):
        return self

    def add_to_shell(self, shell_context):
        pass

    def add_to_process(self, external_context):
        pass

    def teardown_api(self):
        pass


def _get_mapped_size_MB(client):
    status = client.admin.command("serverStatus")

    if "mem" not in status or "mapped" not in status["mem"]:
        raise Exception(
            "Could not get data size of MongoDB server, status was %s" % status)

    return status["mem"]["mapped"]


class SingleMongoDFixture(SimpleFixture):

    """Basic fixture which provides JSTests with a single-MongoD database to connect to.

    Can be restarted automatically after reaching a configurable "mapped" size.

    """

    def __init__(self, logger,
                 mongod_executable=MONGOD_DEFAULT_EXEC,
                 mongod_options={},
                 default_data_path=MONGOD_DEFAULT_DATA_PATH,
                 preserve_dbpath=False,
                 max_mapped_size_MB=None):

        self.logger = logger
        self.mongod_executable = mongod_executable
        self.mongod_options = mongod_options

        self.default_data_path = default_data_path
        self.preserve_dbpath = preserve_dbpath
        self.max_mapped_size_MB = max_mapped_size_MB

        self.mongod = None

    def setup(self):

        if self.mongod is None:
            self.mongod = MongoD(executable=self.mongod_executable,
                                 default_data_path=self.default_data_path,
                                 preserve_dbpath=self.preserve_dbpath,
                                 context=ExternalContext(logger=self.logger),
                                 **self.mongod_options)

        try:
            self.logger.info("Starting MongoDB server...\n%s" % self.mongod)

            self.mongod.start()

            self.logger.info("MongoDB server started at %s:%s with pid %s." %
                             (self.mongod.host, self.mongod.port, self.mongod.pid()))

            self.mongod.wait_for_client()

            self.logger.info("MongoDB server at %s:%s successfully contacted." %
                             (self.mongod.host, self.mongod.port))

            self.mongod.flush()

        except:
            self.logger.error("MongoDB server failed to start.", exc_info=True)
            raise

    def add_to_shell(self, shell_context):
        shell_context.db_address = \
            "%s:%s" % (self.mongod.host, self.mongod.port)

    def teardown_api(self):
        if self.max_mapped_size_MB is not None:
            if _get_mapped_size_MB(self.mongod.client()) > self.max_mapped_size_MB:

                self.logger.info(
                    "Maximum mapped size %sMB reached, restarting MongoDB..." %
                    self.max_mapped_size_MB)

                self.teardown()
                self.setup()

    def teardown(self):

        try:
            self.logger.info("Stopping MongoDB server at %s:%s with pid %s..." %
                             (self.mongod.host, self.mongod.port, self.mongod.pid()))

            self.mongod.stop()

            self.logger.info("MongoDB server stopped.")

        except:
            self.logger.error("MongoDB server failed to stop.", exc_info=True)
            raise


class MasterSlaveFixture(SimpleFixture):

    """Fixture which provides JSTests with a master-MongoD database to connect to.

    A slave MongoD instance replicates the master in the background.

    """

    def __init__(self, logger,
                 mongod_executable=MONGOD_DEFAULT_EXEC,
                 mongod_options={},
                 master_options={},
                 slave_options={},
                 default_data_path=MONGOD_DEFAULT_DATA_PATH,
                 preserve_dbpath=False,
                 max_mapped_size_MB=None):

        self.logger = logger
        self.mongod_executable = mongod_executable

        self.master_options = {}
        self.master_options.update(mongod_options)
        self.master_options.update(master_options)

        self.slave_options = {}
        self.slave_options.update(mongod_options)
        self.slave_options.update(slave_options)

        self.default_data_path = default_data_path
        self.preserve_dbpath = preserve_dbpath
        self.max_mapped_size_MB = max_mapped_size_MB

        self.master = None
        self.slave = None

    def setup(self):

        if self.master is None:

            self.master_options["master"] = ""

            self.master = MongoD(executable=self.mongod_executable,
                                 default_data_path=self.default_data_path,
                                 preserve_dbpath=self.preserve_dbpath,
                                 context=ExternalContext(logger=self.logger),
                                 **self.master_options)

        try:
            self.logger.info("Starting MongoDB master server...\n%s" % self.master)

            self.master.start()

            self.logger.info("MongoDB master server started at %s:%s with pid %s." %
                             (self.master.host, self.master.port, self.master.pid()))

            self.master.wait_for_client()

            self.logger.info("MongoDB master server at %s:%s successfully contacted." %
                             (self.master.host, self.master.port))

            self.master.flush()

        except:
            self.logger.error("MongoDB master server failed to start.", exc_info=True)
            raise

        if self.slave is None:

            self.slave_options["slave"] = ""
            self.slave_options["source"] = "%s:%s" % (self.master.host, self.master.port)

            self.slave = MongoD(executable=self.mongod_executable,
                                default_data_path=self.default_data_path,
                                context=ExternalContext(logger=self.logger),
                                **self.slave_options)

        try:
            self.logger.info("Starting MongoDB slave server...\n%s" % self.slave)

            self.slave.start()

            self.logger.info("MongoDB slave server started at %s:%s with pid %s." %
                             (self.slave.host, self.slave.port, self.slave.pid()))

            self.slave.wait_for_client()

            self.logger.info("MongoDB slave server at %s:%s successfully contacted." %
                             (self.slave.host, self.slave.port))

            self.slave.flush()

        except:
            self.logger.error("MongoDB slave server failed to start.", exc_info=True)
            raise

    def add_to_shell(self, shell_context):
        shell_context.db_address = \
            "%s:%s" % (self.master.host, self.master.port)

    def teardown_api(self):
        if self.max_mapped_size_MB is not None:
            if _get_mapped_size_MB(self.master.client()) > self.max_mapped_size_MB:

                self.logger.info(
                    "Maximum mapped size %sMB reached, restarting MongoDB..." %
                    self.max_mapped_size_MB)

                self.teardown()
                self.setup()

    def teardown(self):

        try:
            self.logger.info("Stopping MongoDB slave server at %s:%s with pid %s..." %
                             (self.slave.host, self.slave.port, self.slave.pid()))

            self.slave.stop()

            self.logger.info("MongoDB slave server stopped.")

        except:
            self.logger.error("MongoDB slave server failed to stop.", exc_info=True)
            raise

        try:
            self.logger.info("Stopping MongoDB master server at %s:%s with pid %s..." %
                             (self.master.host, self.master.port, self.master.pid()))

            self.master.stop()

            self.logger.info("MongoDB master server stopped.")

        except:
            self.logger.error("MongoDB master server failed to stop.", exc_info=True)
            raise


class GlobalShellFixture(SimpleFixture):

    """Passthrough fixture which just allows passing JSON options directly as shell global vars.

    Useful for passing arbitrary options to jstests when running in the shell, for example auth
    options.

    """

    def __init__(self, logger, **kwargs):

        self.logger = logger
        self.kwargs = kwargs

    def setup(self):
        pass

    def add_to_shell(self, shell_context):
        shell_context.global_context.update(self.kwargs)

    def teardown_api(self):
        pass

    def teardown(self):
        pass
