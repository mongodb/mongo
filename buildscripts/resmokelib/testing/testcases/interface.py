"""Subclass of unittest.TestCase with helpers for spawning a separate process.

This is used to perform the actual test case.
"""

import os
import os.path
import sys
import threading
import timeit
import unittest
import uuid
from typing import Any, Callable, Dict, Optional

import psutil

from buildscripts.resmokelib import config, logging
from buildscripts.resmokelib.hang_analyzer.hang_analyzer import HangAnalyzer
from buildscripts.resmokelib.utils import registry
from buildscripts.resmokelib.utils.self_test_fakes import test_analysis

_TEST_CASES: Dict[str, Callable] = {}  # type: ignore


def make_test_case(test_kind, *args, **kwargs) -> "TestCase":
    """Provide factory function for creating TestCase instances."""
    if test_kind not in _TEST_CASES:
        raise ValueError("Unknown test kind '%s'" % test_kind)
    return _TEST_CASES[test_kind](*args, **kwargs)


class TestCase(unittest.TestCase, metaclass=registry.make_registry_metaclass(_TEST_CASES)):
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
        self.timed_out = threading.Event()
        self.timed_out_processed = threading.Event()

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

    def configure(self, fixture: "fixture.Fixture", *args, **kwargs):
        """Store 'fixture' as an attribute for later use during execution."""
        if self.is_configured:
            raise RuntimeError("configure can only be called once")

        self.is_configured = True
        self.fixture = fixture

    def run_test(self):
        """Run the specified test."""
        raise NotImplementedError("run_test must be implemented by TestCase subclasses")

    def on_timeout(self):
        """Invoked when test execution has exceeded its time limit."""
        self.timed_out.set()
        self.timed_out_processed.set()

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


class ProcessTestCase(TestCase):
    """Base class for TestCases that executes an external process."""

    def run_test(self):
        """Run the test."""
        try:
            self.proc = self._make_process()
            self._execute(self.proc)
        except self.failureException:
            raise
        except:
            self.logger.exception(
                "Encountered an error running %s %s", self.test_kind, self.basename()
            )
            raise

    def as_command(self):
        """Return the command invocation used to run the test."""
        try:
            proc = self._make_process()
            return proc.as_command()
        except:
            self.logger.exception(
                "Encountered an error getting command for %s %s", self.test_kind, self.basename()
            )
            raise self.failureException(
                "%s failed when building process command" % (self.short_description(),)
            )

    def _execute(self, process: "process.Process"):
        """Run the specified process."""

        start_time = timeit.default_timer()
        self.logger.info("Starting %s...\n%s", self.short_description(), process.as_command())

        process.start()
        self.logger.info("%s started with pid %s.", self.short_description(), process.pid)
        self.return_code = process.wait()
        finished_time = timeit.default_timer()

        if self.timed_out.is_set():
            self.timed_out_processed.wait()
            raise self.failureException(
                "%s timed out and was killed, pid %s. Duration of process %fs"
                % (
                    self.short_description(),
                    process.pid,
                    finished_time - start_time,
                )
            )

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

    def _get_all_processes(self):
        """
        A best effort collection of all processes involved in the current test:
        - Processes from the fixture.
        - The test process itself.
        - Any child of the test process.
        - Any process in the same process group as the test process and children (Unix only).
        - Any process that contains the environment variable marker the test process was created with (RESMOKE_TEST_ID=...).

        It is possible this will miss orphaned processes created in a new process group on Mac,
        since reading environment variables from arbitrary processes is generally blocked.
        """

        def get_processes_by_pgid(target_pgid):
            processes = []
            for proc in psutil.process_iter():
                try:
                    if os.getpgid(proc.pid) == target_pgid:
                        processes.append(proc)
                except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
                    pass
            return processes

        def get_processes_with_env(env, value):
            processes = []
            for proc in psutil.process_iter():
                try:
                    if env in proc.environ() and proc.environ().get(env) == value:
                        processes.append(proc)
                except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
                    pass
            return processes

        pids = set([self.proc.pid])

        if self.fixture:
            pids.update(self.fixture.pids())

        processes_children = psutil.Process(self.proc.pid).children(recursive=True)
        pids.update([p.pid for p in processes_children])

        processes_with_marker = get_processes_with_env("RESMOKE_TEST_ID", str(self._id))
        pids.update([p.pid for p in processes_with_marker])

        if sys.platform != "win32":  # getpgid is Unix only
            for child in [*processes_children, self.proc]:
                try:
                    # Only kill the entire process group if the child process was created in a new group.
                    # If it is the same as this process, the group contains resmoke's python process and
                    # those from other parallel jobs.
                    pgid_child = os.getpgid(child.pid)
                    if pgid_child != os.getpgid(0):
                        for p in get_processes_by_pgid(pgid_child):
                            pids.add(p.pid)
                except ProcessLookupError:
                    continue
        return pids

    def on_timeout(self):
        self.timed_out.set()

        pids = self._get_all_processes()

        if "test_analysis" in config.INTERNAL_PARAMS:
            test_analysis(self.logger, pids)
        else:
            options = {
                "dump_core": True,
                "process_ids": ",".join([str(p) for p in pids]),
                "kill_processes": True,
                "debugger_output": "",
                "process_match": "exact",
                "max_disk_usage_percent": 90,
                "go_process_names": "",
                "process_names": "",
            }
            hang_analyzer = HangAnalyzer(options, task_id=None, logger=self.logger)
            hang_analyzer.execute()
        self.timed_out_processed.set()


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


def append_process_tracking_options(kwargs, test_id):
    """Append process kwargs that will enable tracking subprocesses created by this test."""

    # This is leveraged by test timeouts. Since we would like to not apply them in processes where
    # there are nested resmoke invocations, only apply them when a test timeout is set.
    if config.TEST_TIMEOUT is not None:
        kwargs.setdefault("env_vars", {})
        kwargs["env_vars"]["RESMOKE_TEST_ID"] = str(test_id)
        kwargs["start_new_session"] = True
