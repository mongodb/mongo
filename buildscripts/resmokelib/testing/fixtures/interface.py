"""
Interface of the different fixtures for executing JSTests against.
"""

from __future__ import absolute_import

import os.path
import time

import pymongo
import pymongo.errors

from ... import config
from ... import errors
from ... import logging
from ... import utils
from ...utils import registry


_FIXTURES = {}


def make_fixture(class_name, *args, **kwargs):
    """
    Factory function for creating Fixture instances.
    """

    if class_name not in _FIXTURES:
        raise ValueError("Unknown fixture class '%s'" % (class_name))
    return _FIXTURES[class_name](*args, **kwargs)


class Fixture(object):
    """
    Base class for all fixtures.
    """

    __metaclass__ = registry.make_registry_metaclass(_FIXTURES)

    # We explicitly set the 'REGISTERED_NAME' attribute so that PyLint realizes that the attribute
    # is defined for all subclasses of Fixture.
    REGISTERED_NAME = "Fixture"

    def __init__(self, logger, job_num, dbpath_prefix=None):
        """
        Initializes the fixture with a logger instance.
        """

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        if not isinstance(job_num, int):
            raise TypeError("job_num must be an integer")
        elif job_num < 0:
            raise ValueError("job_num must be a nonnegative integer")

        self.logger = logger
        self.job_num = job_num

        dbpath_prefix = utils.default_if_none(config.DBPATH_PREFIX, dbpath_prefix)
        dbpath_prefix = utils.default_if_none(dbpath_prefix, config.DEFAULT_DBPATH_PREFIX)
        self._dbpath_prefix = os.path.join(dbpath_prefix, "job{}".format(self.job_num))

    def setup(self):
        """
        Creates the fixture.
        """
        pass

    def await_ready(self):
        """
        Blocks until the fixture can be used for testing.
        """
        pass

    def teardown(self, finished=False):
        """
        Destroys the fixture. Return true if was successful, and false
        otherwise.

        The fixture's logging handlers are closed if 'finished' is true,
        which should happen when setup() won't be called again.
        """

        try:
            return self._do_teardown()
        finally:
            if finished:
                for handler in self.logger.handlers:
                    # We ignore the cancellation token returned by close_later() since we always
                    # want the logs to eventually get flushed.
                    logging.flush.close_later(handler)

    def _do_teardown(self):
        """
        Destroys the fixture. Return true if was successful, and false
        otherwise.
        """
        return True

    def is_running(self):
        """
        Returns true if the fixture is still operating and more tests
        can be run, and false otherwise.
        """
        return True

    def get_dbpath_prefix(self):
        return self._dbpath_prefix

    def get_internal_connection_string(self):
        """
        Returns the connection string for this fixture. This is NOT a
        driver connection string, but a connection string of the format
        expected by the mongo::ConnectionString class.
        """
        raise NotImplementedError("get_connection_string must be implemented by Fixture subclasses")

    def get_driver_connection_url(self):
        """
        Return the mongodb connection string as defined here:
        https://docs.mongodb.com/manual/reference/connection-string/
        """
        raise NotImplementedError(
            "get_driver_connection_url must be implemented by Fixture subclasses")

    def mongo_client(self, read_preference=pymongo.ReadPreference.PRIMARY, timeout_millis=30000):
        """
        Returns a pymongo.MongoClient connecting to this fixture with a read
        preference of 'read_preference'.

        The PyMongo driver will wait up to 'timeout_millis' milliseconds
        before concluding that the server is unavailable.
        """

        kwargs = {"connectTimeoutMS": timeout_millis}
        if pymongo.version_tuple[0] >= 3:
            kwargs["serverSelectionTimeoutMS"] = timeout_millis
            kwargs["connect"] = True

        return pymongo.MongoClient(host=self.get_driver_connection_url(),
                                   read_preference=read_preference,
                                   **kwargs)

    def __str__(self):
        return "%s (Job #%d)" % (self.__class__.__name__, self.job_num)

    def __repr__(self):
        return "%r(%r, %r)" % (self.__class__.__name__, self.logger, self.job_num)


class ReplFixture(Fixture):
    """
    Base class for all fixtures that support replication.
    """

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    AWAIT_REPL_TIMEOUT_MINS = 5

    def get_primary(self):
        """
        Returns the primary of a replica set, or the master of a
        master-slave deployment.
        """
        raise NotImplementedError("get_primary must be implemented by ReplFixture subclasses")

    def get_secondaries(self):
        """
        Returns a list containing the secondaries of a replica set, or
        the slave of a master-slave deployment.
        """
        raise NotImplementedError("get_secondaries must be implemented by ReplFixture subclasses")

    def retry_until_wtimeout(self, insert_fn):
        """
        Given a callback function representing an insert operation on
        the primary, handle any connection failures, and keep retrying
        the operation for up to 'AWAIT_REPL_TIMEOUT_MINS' minutes.

        The insert operation callback should take an argument for the
        number of remaining seconds to provide as the timeout for the
        operation.
        """

        deadline = time.time() + ReplFixture.AWAIT_REPL_TIMEOUT_MINS * 60

        while True:
            try:
                remaining = deadline - time.time()
                insert_fn(remaining)
                break
            except pymongo.errors.ConnectionFailure:
                remaining = deadline - time.time()
                if remaining <= 0.0:
                    raise errors.ServerFailure(
                        "Failed to connect to ".format(self.get_driver_connection_url()))


class NoOpFixture(Fixture):
    """A Fixture implementation that does not start any servers.

    Used when the MongoDB deployment is started by the JavaScript test itself with MongoRunner,
    ReplSetTest, or ShardingTest.
    """

    REGISTERED_NAME = "NoOpFixture"

    def get_internal_connection_string(self):
        return None

    def get_driver_connection_url(self):
        return None
