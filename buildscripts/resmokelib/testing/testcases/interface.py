"""Subclass of unittest.TestCase with helpers for spawning a separate process.

This is used to perform the actual test case.
"""
import glob
import os
import os.path
import unittest
import uuid

from buildscripts.resmokelib import logging
from buildscripts.resmokelib.utils import registry

_TEST_CASES = {}  # type: ignore


def make_test_case(test_kind, *args, **kwargs):
    """Provide factory function for creating TestCase instances."""
    if test_kind not in _TEST_CASES:
        raise ValueError("Unknown test kind '%s'" % test_kind)
    return _TEST_CASES[test_kind](*args, **kwargs)


class TestCase(unittest.TestCase, metaclass=registry.make_registry_metaclass(_TEST_CASES)):  # pylint: disable=invalid-metaclass
    """A test case to execute."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, logger, test_kind, test_name, dynamic=False):
        """Initialize the TestCase with the name of the test."""
        unittest.TestCase.__init__(self, methodName="run_test")

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        if not isinstance(test_kind, str):
            raise TypeError("test_kind must be a string")

        if not isinstance(test_name, str):
            raise TypeError("test_name must be a string")

        self._id = uuid.uuid4()

        # When the TestCase is created by the TestSuiteExecutor (through a call to make_test_case())
        # logger is an instance of TestQueueLogger. When the TestCase is created by a hook
        # implementation it is an instance of BaseLogger.
        self.logger = logger
        # Used to store the logger when overridden by a test logger in Report.start_test().
        self._original_logger = None

        self.test_kind = test_kind
        self.test_name = test_name
        self.dynamic = dynamic

        self.fixture = None
        self.return_code = None
        self.propagate_error = None

        self.is_configured = False

    def long_name(self):
        """Return the path to the test, relative to the current working directory."""
        return os.path.relpath(self.test_name)

    def basename(self):
        """Return the basename of the test."""
        return os.path.basename(self.test_name)

    def short_name(self):
        """Return the basename of the test without the file extension."""
        return os.path.splitext(self.basename())[0]

    def id(self):
        """Return the id of the test."""
        return self._id

    def short_description(self):
        """Return the short_description of the test."""
        return "%s %s" % (self.test_kind, self.test_name)

    def override_logger(self, new_logger):
        """Override this instance's logger with a new logger.

        This method is used by the repport to set the test logger.
        """
        assert not self._original_logger, "Logger already overridden"
        self._original_logger = self.logger
        self.logger = new_logger

    def reset_logger(self):
        """Reset this instance's logger to its original value."""
        assert self._original_logger, "Logger was not overridden"
        self.logger = self._original_logger
        self._original_logger = None

    def configure(self, fixture, *args, **kwargs):  # pylint: disable=unused-argument
        """Store 'fixture' as an attribute for later use during execution."""
        if self.is_configured:
            raise RuntimeError("configure can only be called once")

        self.is_configured = True
        self.fixture = fixture

    def run_test(self):
        """Run the specified test."""
        raise NotImplementedError("run_test must be implemented by TestCase subclasses")

    def as_command(self):
        """Return the command invocation used to run the test or None."""
        return None


class UndoDBUtilsMixin:
    """Utility functions for interacting with UndoDB."""

    def __init__(self, logger, *args, **kwargs):  # pylint: disable=unused-argument
        """Initialize the mixin to resember a TestCase."""
        self.logger = logger

    def _cull_recordings(self, program_executable):
        """Move recordings if test fails so it doesn't get deleted."""
        # Only store my recordings. Concurrent processes may generate their own recordings that we
        # should ignore. There's a problem with duplicate program names under different directories
        # But that should be rare and there's no harm in having more recordings stored.
        for recording in glob.glob(program_executable + "*.undo"):
            self.logger.info("Keeping recording %s", recording)
            os.rename(recording, recording + '.tokeep')


class ProcessTestCase(TestCase, UndoDBUtilsMixin):
    """Base class for TestCases that executes an external process."""

    def run_test(self):
        """Run the test."""
        try:
            proc = self._make_process()
            self._execute(proc)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running %s %s", self.test_kind,
                                  self.basename())
            raise

    def as_command(self):
        """Return the command invocation used to run the test."""
        return self._make_process().as_command()

    def _execute(self, process):
        """Run the specified process."""
        self.logger.info("Starting %s...\n%s", self.short_description(), process.as_command())

        process.start()
        self.logger.info("%s started with pid %s.", self.short_description(), process.pid)

        self.return_code = process.wait()
        if self.return_code != 0:
            raise self.failureException("%s failed" % (self.short_description()))

        self.logger.info("%s finished.", self.short_description())

    def _make_process(self):
        """Return a new Process instance that could be used to run the test or log the command."""
        raise NotImplementedError("_make_process must be implemented by TestCase subclasses")
