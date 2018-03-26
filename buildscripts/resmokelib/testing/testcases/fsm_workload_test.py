"""The unittest.TestCase for FSM workloads."""

from __future__ import absolute_import

from buildscripts.resmokelib.testing.testcases import jsrunnerfile


class FSMWorkloadTestCase(jsrunnerfile.JSRunnerFileTestCase):
    """An FSM workload to execute."""

    REGISTERED_NAME = "fsm_workload_test"

    def __init__(self, logger, fsm_workload, shell_executable=None, shell_options=None):
        """Initialize the FSMWorkloadTestCase with the FSM workload file."""

        jsrunnerfile.JSRunnerFileTestCase.__init__(
            self, logger, "FSM workload", fsm_workload,
            test_runner_file="jstests/concurrency/fsm_libs/resmoke_runner.js",
            shell_executable=shell_executable, shell_options=shell_options)

    @property
    def fsm_workload(self):
        """Get the test name."""
        return self.test_name

    def _populate_test_data(self, test_data):
        test_data["fsmWorkloads"] = self.fsm_workload
