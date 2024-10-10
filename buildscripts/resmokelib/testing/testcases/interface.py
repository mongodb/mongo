"""Subclass of unittest.TestCase with helpers for spawning a separate process.

This is used to perform the actual test case.
"""

import glob
import os
import os.path
import timeit
import unittest
import uuid
from typing import Any, Callable, Dict, Optional

from buildscripts.resmokelib import logging
from buildscripts.resmokelib.utils import registry

_TEST_CASES: Dict[str, Callable] = {}  # type: ignore


def make_test_case(test_kind, *args, **kwargs) -> "TestCase":
    """Provide factory function for creating TestCase instances."""
    if test_kind not in _TEST_CASES:
        raise ValueError("Unknown test kind '%s'" % test_kind)
    return _TEST_CASES[test_kind](*args, **kwargs)


class TestCase(unittest.TestCase, metaclass=registry.make_registry_metaclass(_TEST_CASES)):  # pylint: disable=invalid-metaclass
    """A test case to execute."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(
        self, logger: logging.Logger, test_kind: str, test_name: str, dynamic: bool = False
    ):
        """Initialize the TestCase with the name of the test."""
        unittest.TestCase.__init__(self, methodName="run_test")

        if not isinstance(logger, logging.Logger):
            raise TypeError("logger must be a Logger instance")

        if not isinstance(test_kind, str):
            raise TypeError("test_kind must be a string")

        if not isinstance(test_name, str):
            raise TypeError(f"test_name must be a string instead it is {type(test_name)}")

        self._id = uuid.uuid4()

        # When the TestCase is created by the TestSuiteExecutor (through a call to make_test_case())
        # logger is an instance of TestQueueLogger. When the TestCase is created by a hook
        # implementation it is an instance of BaseLogger.
        self.logger = logger
        # Used to store the logger when overridden by a test logger in Report.start_test().
        self._original_logger: Optional[logging.Logger] = None

        self.test_kind = test_kind
        self.test_name = test_name
        self.dynamic = dynamic

        self.fixture: Optional["fixture.Fixture"] = None
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

    def get_test_kind(self):
        """Return the kind of the test. This will be something like JSTest."""
        return self.test_kind

    def short_description(self):
        """Return the short_description of the test."""
        return "%s %s" % (self.test_kind, self.test_name)

    def override_logger(self, new_logger: logging.Logger):
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

    def configure(self, fixture: "fixture.Fixture", *args, **kwargs):  # pylint: disable=unused-argument
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

    class METRIC_NAMES:
        BASE_NAME = "test_base_name"
        LONG_NAME = "test_long_name"
        ID = "test_id"
        KIND = "test_kind"
        DYNAMIC = "test_dynamic"
        BACKGROUND = "test_background"

    def get_test_otel_attributes(self) -> Dict[str, Any]:
        return {
            TestCase.METRIC_NAMES.BASE_NAME: self.basename(),
            TestCase.METRIC_NAMES.LONG_NAME: self.long_name(),
            TestCase.METRIC_NAMES.ID: str(self.id()),
            TestCase.METRIC_NAMES.KIND: self.get_test_kind(),
            TestCase.METRIC_NAMES.DYNAMIC: self.dynamic,
        }


class UndoDBUtilsMixin:
    """Utility functions for interacting with UndoDB."""

    def __init__(self, logger: logging.Logger, *args, **kwargs):  # pylint: disable=unused-argument
        """Initialize the mixin to resember a TestCase."""
        self.logger = logger

    def _cull_recordings(self, program_executable):
        """Move recordings if test fails so it doesn't get deleted."""
        # Only store my recordings. Concurrent processes may generate their own recordings that we
        # should ignore. There's a problem with duplicate program names under different directories
        # But that should be rare and there's no harm in having more recordings stored.
        for recording in glob.glob(program_executable + "*.undo"):
            self.logger.info("Keeping recording %s", recording)
            os.rename(recording, recording + ".tokeep")


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
            self.logger.exception(
                "Encountered an error running %s %s", self.test_kind, self.basename()
            )
            raise

    def as_command(self):
        """Return the command invocation used to run the test."""
        return self._make_process().as_command()

    def _execute(self, process: "process.Process"):
        """Run the specified process."""

        start_time = timeit.default_timer()
        self.logger.info("Starting %s...\n%s", self.short_description(), process.as_command())

        process.start()
        self.logger.info("%s started with pid %s.", self.short_description(), process.pid)
        self.return_code = process.wait()
        finished_time = timeit.default_timer()
        if self.return_code != 0:
            raise self.failureException(
                "%s failed with exit code %s, pid %s. Duration of process %fs"
                % (
                    self.short_description(),
                    self.return_code,
                    process.pid,
                    finished_time - start_time,
                )
            )

        self.logger.info(
            "%s finished. Duration of process %fs",
            self.short_description(),
            finished_time - start_time,
        )

    def _make_process(self) -> "process.Process":
        """Return a new Process instance that could be used to run the test or log the command."""
        raise NotImplementedError("_make_process must be implemented by TestCase subclasses")


class TestCaseFactory:
    def __init__(self, factory_class, shell_options):
        if not issubclass(factory_class, TestCase):
            raise TypeError(
                "factory_class should be a subclass of Interface.TestCase", factory_class
            )
        self._factory_class = factory_class
        self.shell_options = shell_options

    def create_test_case(self, logger: logging.Logger, shell_options) -> TestCase:
        raise NotImplementedError(
            "create_test_case must be implemented by TestCaseFactory subclasses"
        )

    def create_test_case_for_thread(
        self,
        logger: logging.Logger,
        num_clients: int = 1,
        thread_id: int = 0,
        tenant_id: Optional[str] = None,
    ) -> TestCase:
        """Create and configure a TestCase to be run in a separate thread."""

        shell_options = self._get_shell_options_for_thread(num_clients, thread_id, tenant_id)
        test_case = self.create_test_case(logger, shell_options)
        return test_case

    def configure(self, fixture: "fixture.Fixture", *args, **kwargs):
        """Configure the test case factory."""
        raise NotImplementedError("configure must be implemented by TestCaseFactory subclasses")

    def make_process(self):
        """Make a process for a TestCase."""
        raise NotImplementedError("make_process must be implemented by TestCaseFactory subclasses")

    def _get_shell_options_for_thread(
        self, num_clients: int, thread_id: int, tenant_id: Optional[str]
    ) -> dict:
        """Get shell_options with an initialized TestData object for given thread."""

        # We give each thread its own copy of the shell_options.
        shell_options = self.shell_options.copy()
        global_vars = shell_options["global_vars"].copy()
        test_data = global_vars["TestData"].copy()
        if tenant_id:
            test_data["tenantId"] = tenant_id

        # We set a property on TestData to mark the main test when multiple clients are going to run
        # concurrently in case there is logic within the test that must execute only once. We also
        # set a property on TestData to indicate how many clients are going to run the test so they
        # can avoid executing certain logic when there may be other operations running concurrently.
        is_main_test = thread_id == 0
        test_data["isMainTest"] = is_main_test
        test_data["numTestClients"] = num_clients

        global_vars["TestData"] = test_data
        shell_options["global_vars"] = global_vars

        return shell_options
