"""
Interface of the different fixtures for executing JSTests against.
"""

from __future__ import absolute_import

from ... import logging


class Fixture(object):
    """
    Base class for all fixtures.
    """

    def __init__(self, logger, job_num):
        """
        Initializes the fixtures with a logger instance.
        """

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        if not isinstance(job_num, int):
            raise TypeError("job_num must be an integer")
        elif job_num < 0:
            raise ValueError("job_num must be a nonnegative integer")

        self.logger = logger
        self.job_num = job_num

        self.port = None  # Port that the mongo shell should connect to.

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

    def teardown(self):
        """
        Destroys the fixture. Return true if was successful, and false otherwise.
        """
        return True

    def is_running(self):
        """
        Returns true if the fixture is still operating and more tests
        can be run, and false otherwise.
        """
        return True

    def __str__(self):
        return "%s (Job #%d)" % (self.__class__.__name__, self.job_num)

    def __repr__(self):
        return "%r(%r, %r)" % (self.__class__.__name__, self.logger, self.job_num)


class ReplFixture(Fixture):
    """
    Base class for all fixtures that support replication.
    """

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

    def await_repl(self):
        """
        Blocks until all operations on the primary/master have
        replicated to all other nodes.
        """
        raise NotImplementedError("await_repl must be implemented by ReplFixture subclasses")
