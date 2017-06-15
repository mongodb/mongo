"""
Subclass of unittest.TestCase with helpers for spawning a separate
process to perform the actual test case.
"""

from __future__ import absolute_import

import os
import os.path
import unittest

from ... import config
from ... import logging
from ...utils import registry


_TEST_CASES = {}


def make_test_case(test_kind, *args, **kwargs):
    """
    Factory function for creating TestCase instances.
    """

    if test_kind not in _TEST_CASES:
        raise ValueError("Unknown test kind '%s'" % (test_kind))
    return _TEST_CASES[test_kind](*args, **kwargs)


class TestCase(unittest.TestCase):
    """
    A test case to execute.
    """

    __metaclass__ = registry.make_registry_metaclass(_TEST_CASES)

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, logger, test_kind, test_name):
        """
        Initializes the TestCase with the name of the test.
        """

        unittest.TestCase.__init__(self, methodName="run_test")

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        if not isinstance(test_kind, basestring):
            raise TypeError("test_kind must be a string")

        if not isinstance(test_name, basestring):
            raise TypeError("test_name must be a string")

        # When the TestCase is created by the TestSuiteExecutor (through a call to make_test_case())
        # logger is an instance of TestQueueLogger. When the TestCase is created by a hook
        # implementation it is an instance of BaseLogger.
        self.logger = logger
        self.test_kind = test_kind
        self.test_name = test_name

        self.fixture = None
        self.return_code = None

        self.is_configured = False

    def long_name(self):
        """
        Returns the path to the test, relative to the current working directory.
        """
        return os.path.relpath(self.test_name)

    def basename(self):
        """
        Returns the basename of the test.
        """
        return os.path.basename(self.test_name)

    def short_name(self):
        """
        Returns the basename of the test without the file extension.
        """
        return os.path.splitext(self.basename())[0]

    def id(self):
        return self.test_name

    def shortDescription(self):
        return "%s %s" % (self.test_kind, self.test_name)

    def configure(self, fixture, *args, **kwargs):
        """
        Stores 'fixture' as an attribute for later use during execution.
        """
        if self.is_configured:
            raise RuntimeError("configure can only be called once")

        self.is_configured = True
        self.fixture = fixture

    def run_test(self):
        """
        Runs the specified test.
        """
        raise NotImplementedError("run_test must be implemented by TestCase subclasses")

    def as_command(self):
        """
        Returns the command invocation used to run the test.
        """
        return self._make_process().as_command()

    def _execute(self, process):
        """
        Runs the specified process.
        """

        if config.INTERNAL_EXECUTOR_NAME is not None:
            self.logger.info("Starting %s under executor %s...\n%s",
                             self.shortDescription(),
                             config.INTERNAL_EXECUTOR_NAME,
                             process.as_command())
        else:
            self.logger.info("Starting %s...\n%s", self.shortDescription(), process.as_command())

        process.start()
        self.logger.info("%s started with pid %s.", self.shortDescription(), process.pid)

        self.return_code = process.wait()
        if self.return_code != 0:
            raise self.failureException("%s failed" % (self.shortDescription()))

        self.logger.info("%s finished.", self.shortDescription())

    def _make_process(self):
        """
        Returns a new Process instance that could be used to run the
        test or log the command.
        """
        raise NotImplementedError("_make_process must be implemented by TestCase subclasses")
