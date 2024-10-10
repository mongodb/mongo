"""The unittest.TestCase for FSM workloads."""

import hashlib
import threading
import uuid
from typing import Optional

from buildscripts.resmokelib import logging, utils
from buildscripts.resmokelib.testing.testcases import interface, jsrunnerfile, jstest
from buildscripts.resmokelib.utils import registry


class _SingleFSMWorkloadTestCase(jsrunnerfile.JSRunnerFileTestCase):
    """An FSM workload to execute."""

    REGISTERED_NAME = registry.LEAVE_UNREGISTERED

    def __init__(self, logger, test_name, test_id, shell_executable=None, shell_options=None):
        """Initialize the _SingleFSMWorkloadTestCase with the FSM workload file."""

        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self,
            logger,
            "FSM workload",
            test_name,
            test_runner_file="jstests/concurrency/fsm_libs/resmoke_runner.js",
            shell_executable=shell_executable,
            shell_options=shell_options,
        )
        self._id = test_id

    def configure(self, fixture, *args, **kwargs):
        """Configure the FSMWorkloadTestCase runner."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)


class _FSMWorkloadTestCaseBuilder(interface.TestCaseFactory):
    """Build the real TestCase in the FSMWorkloadTestCase wrapper."""

    _COUNTER_LOCK = threading.Lock()
    _COUNTER = 0

    def __init__(
        self,
        logger,
        fsm_workload_group,
        test_name,
        test_id,
        shell_executable=None,
        shell_options=None,
        same_db=False,
        same_collection=False,
        db_name_prefix=None,
    ):
        """Initialize the _FSMWorkloadTestCaseBuilder."""
        interface.TestCaseFactory.__init__(self, _SingleFSMWorkloadTestCase, shell_options)
        self.logger = logger
        self.fsm_workload_group = fsm_workload_group
        self.test_name = test_name
        self.test_id = test_id
        self.shell_executable = shell_executable
        self.shell_options = utils.default_if_none(shell_options, {}).copy()

        self.same_collection = same_collection
        self.same_db = same_db or self.same_collection
        self.db_name_prefix = db_name_prefix
        self.dbpath_prefix = None

        self.fixture = None

    def configure(self, fixture, *args, **kwargs):
        self.fixture = fixture
        self.dbpath_prefix = self.fixture.get_dbpath_prefix()

        global_vars = self.shell_options.get("global_vars", {}).copy()

        test_data = global_vars.get("TestData", {}).copy()
        self._populate_test_data(test_data)

        global_vars["TestData"] = test_data
        self.shell_options["global_vars"] = global_vars

    def _populate_test_data(self, test_data):
        test_data["fsmWorkloads"] = self.fsm_workload_group
        test_data["resmokeDbPathPrefix"] = self.dbpath_prefix

        with _FSMWorkloadTestCaseBuilder._COUNTER_LOCK:
            count = _FSMWorkloadTestCaseBuilder._COUNTER
            _FSMWorkloadTestCaseBuilder._COUNTER += 1

        # We use a global incrementing counter as a prefix for the database name to avoid any
        # collection lifecycle related issues in sharded clusters. This more closely matches how
        # uniqueDBName() and uniqueCollName() would have returned distinct values when called once
        # for each FSM workload in the entire schedule by runner.js.
        test_prefix = self.db_name_prefix if self.db_name_prefix else "test"
        test_data["dbNamePrefix"] = "{}{:d}_".format(test_prefix, count)
        test_data["sameDB"] = self.same_db
        test_data["sameCollection"] = self.same_collection
        test_data["peerPids"] = self.fixture.pids()

    def make_process(self):
        # This function should only be called by MultiClientsTestCase's _make_process().
        test_case = self.create_test_case(self.logger, self.shell_options)
        return test_case._make_process()  # pylint: disable=protected-access

    def create_test_case(self, logger, shell_options) -> _SingleFSMWorkloadTestCase:
        test_case = _SingleFSMWorkloadTestCase(
            logger, self.test_name, self.test_id, self.shell_executable, shell_options
        )
        test_case.configure(self.fixture)
        return test_case


class FSMWorkloadTestCase(jstest.MultiClientsTestCase):
    """A wrapper for several copies of a _SingleFSMWorkloadTestCase to execute."""

    REGISTERED_NAME = "fsm_workload_test"
    TEST_KIND = "FSM workload"

    def __init__(
        self,
        logger: logging.Logger,
        selected_tests: list[str],
        shell_executable: Optional[str] = None,
        shell_options: Optional[dict] = None,
        same_db: bool = False,
        same_collection: bool = False,
        db_name_prefix: Optional[str] = None,
    ):
        """Initialize the FSMWorkloadTestCase with the FSM workload file."""
        assert len(selected_tests) == 1
        fsm_workload_group = self.get_workload_group(selected_tests[0])
        test_name = self.get_workload_uid(selected_tests[0])
        test_id = uuid.uuid4()

        factory = _FSMWorkloadTestCaseBuilder(
            logger,
            fsm_workload_group,
            test_name,
            test_id,
            shell_executable,
            shell_options,
            same_db,
            same_collection,
            db_name_prefix,
        )
        jstest.MultiClientsTestCase.__init__(
            self, logger, self.TEST_KIND, test_name, test_id, factory
        )

    @staticmethod
    def get_workload_group(selected_test: str) -> list[str]:
        """Generate an FSM workload group from tests selected by the selector."""
        # Selectors for non-parallel FSM suites return the name of a workload, we
        # put it into a list to create a workload group of size 1.
        return [selected_test]

    @staticmethod
    def get_workload_uid(selected_test: str) -> str:
        """Get an unique identifier for a workload group."""
        # For non-parallel versions of the FSM framework, the workload group name is just the
        # name of the workload.
        return selected_test


class ParallelFSMWorkloadTestCase(FSMWorkloadTestCase):
    """An FSM workload to execute."""

    REGISTERED_NAME = "parallel_fsm_workload_test"

    @staticmethod
    def get_workload_group(selected_tests: list[str]) -> list[str]:  # pylint: disable=arguments-renamed
        """Generate an FSM workload group from tests selected by the selector.

        When this function was updated the naming was misleading.
        Now the naming is right and mypy warns that that this is a different function than its parent which is correct.
        This should be refactored when time permits.

        Just return the list of selected tests as the workload.

        Args:
            selected_tests (list[str]): list of tests to use

        Returns
            list[str]: selected tests
        """
        return selected_tests

    @staticmethod
    def get_workload_uid(selected_tests: list[str]) -> str:  # pylint: disable=arguments-renamed
        """Get an unique identifier for a workload group.

        When this function was updated the naming was misleading.
        Now the naming is right and mypy warns that that this is a different function than its parent which is correct.
        This should be refactored when time permits.

        Args:
            selected_tests (list[str]): list of tests to use

        Returns
            str: hash of all tests
        """
        uid = hashlib.md5()
        for workload_name in sorted(selected_tests):
            uid.update(workload_name.encode("utf-8"))
        return uid.hexdigest()
