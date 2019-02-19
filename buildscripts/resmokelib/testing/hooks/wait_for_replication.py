"""Test hook to wait for replication to complete on a replica set."""

import time

from buildscripts.resmokelib import core
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import interface


class WaitForReplication(interface.Hook):
    """Wait for replication to complete."""

    def __init__(self, hook_logger, fixture):
        """Initialize WaitForReplication."""
        description = "WaitForReplication waits on a replica set"
        interface.Hook.__init__(self, hook_logger, fixture, description)

        self.hook_logger = hook_logger
        self.fixture = fixture

    def after_test(self, test, test_report):
        """Run mongo shell to call replSetTest.awaitReplication()."""
        start_time = time.time()
        client_conn = self.fixture.get_driver_connection_url()
        js_cmds = "conn = '{}'; rst = new ReplSetTest(conn); rst.awaitReplication();".format(
            client_conn)
        shell_options = {"nodb": "", "eval": js_cmds}
        shell_proc = core.programs.mongo_shell_program(self.hook_logger, **shell_options)
        shell_proc.start()
        return_code = shell_proc.wait()
        if return_code:
            raise errors.ServerFailure("Awaiting replication failed for {}".format(client_conn))
        self.hook_logger.info("WaitForReplication took %0.4f seconds", time.time() - start_time)
