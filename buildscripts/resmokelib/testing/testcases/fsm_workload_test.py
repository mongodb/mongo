"""The unittest.TestCase for FSM workloads."""

import hashlib
import threading

from buildscripts.resmokelib.testing.testcases import interface
from buildscripts.resmokelib.testing.testcases import jsrunnerfile


class FSMWorkloadTestCase(jsrunnerfile.JSRunnerFileTestCase):
    """An FSM workload to execute."""

    REGISTERED_NAME = "fsm_workload_test"

    _COUNTER_LOCK = threading.Lock()
    _COUNTER = 0

    def __init__(self, logger, selected_tests, shell_executable=None, shell_options=None,
                 same_db=False, same_collection=False, db_name_prefix=None):
        """Initialize the FSMWorkloadTestCase with the FSM workload file."""

        self.same_collection = same_collection
        self.same_db = same_db or self.same_collection
        self.db_name_prefix = db_name_prefix
        self.dbpath_prefix = None
        self.fsm_workload_group = self.get_workload_group(selected_tests)

        test_name = self.get_workload_uid(selected_tests)

        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self, logger, "FSM workload", test_name,
            test_runner_file="jstests/concurrency/fsm_libs/resmoke_runner.js",
            shell_executable=shell_executable, shell_options=shell_options)

    def configure(self, fixture, *args, **kwargs):
        """Configure the FSMWorkloadTestCase runner."""
        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)

        self.dbpath_prefix = self.fixture.get_dbpath_prefix()

        global_vars = self.shell_options.get("global_vars", {}).copy()

        test_data = global_vars.get("TestData", {}).copy()
        self._populate_test_data(test_data)

        global_vars["TestData"] = test_data
        self.shell_options["global_vars"] = global_vars

    def _populate_test_data(self, test_data):
        test_data["fsmWorkloads"] = self.fsm_workload_group
        test_data["resmokeDbPathPrefix"] = self.dbpath_prefix

        with FSMWorkloadTestCase._COUNTER_LOCK:
            count = FSMWorkloadTestCase._COUNTER
            FSMWorkloadTestCase._COUNTER += 1

        # We use a global incrementing counter as a prefix for the database name to avoid any
        # collection lifecycle related issues in sharded clusters. This more closely matches how
        # uniqueDBName() and uniqueCollName() would have returned distinct values when called once
        # for each FSM workload in the entire schedule by runner.js.
        test_prefix = self.db_name_prefix if self.db_name_prefix else "test"
        test_data["dbNamePrefix"] = "{}{:d}_".format(test_prefix, count)
        test_data["sameDB"] = self.same_db
        test_data["sameCollection"] = self.same_collection
        test_data["peerPids"] = self.fixture.pids()

    @staticmethod
    def get_workload_group(selected_tests):
        """Generate an FSM workload group from tests selected by the selector."""
        # Selectors for non-parallel FSM suites return the name of a workload, we
        # put it into a list to create a workload group of size 1.
        return [selected_tests]

    @staticmethod
    def get_workload_uid(selected_tests):
        """Get an unique identifier for a workload group."""
        # For non-parallel versions of the FSM framework, the workload group name is just the
        # name of the workload.
        return selected_tests


class ParallelFSMWorkloadTestCase(FSMWorkloadTestCase):
    """An FSM workload to execute."""

    REGISTERED_NAME = "parallel_fsm_workload_test"

    @staticmethod
    def get_workload_group(selected_tests):
        """Generate an FSM workload group from tests selected by the selector."""
        # Just return the list of selected tests as the workload.
        return selected_tests

    @staticmethod
    def get_workload_uid(selected_tests):
        """Get an unique identifier for a workload group."""
        uid = hashlib.md5()
        for workload_name in sorted(selected_tests):
            uid.update(workload_name.encode("utf-8"))
        return uid.hexdigest()
