"""Test hook to wait for replication to complete on a replica set."""

import time

from buildscripts.resmokelib import core, errors
from buildscripts.resmokelib.testing.hooks import interface


class WaitForReplication(interface.Hook):
    """Wait for replication to complete."""

    IS_BACKGROUND = False

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
        js_cmds = """
            const {{ReplSetTest}} = await import("jstests/libs/replsettest.js");
            const conn = '{}';
            try {{
                const rst = new ReplSetTest(conn);
                rst.awaitReplication();
            }} catch (e) {{
                jsTestLog("WaitForReplication got error: " + tojson(e));
                if (!e.message.includes('The server is in quiesce mode and will shut down')) {{
                    throw e;
                }}
                jsTestLog("Ignoring shutdown error in quiesce mode");
            }}"""
        shell_options = {"nodb": "", "eval": js_cmds.format(client_conn)}
        shell_proc = core.programs.mongo_shell_program(
            self.hook_logger,
            test_name="wait_for_replication",
            **shell_options,
        )
        shell_proc.start()
        return_code = shell_proc.wait()
        if return_code:
            raise errors.ServerFailure("Awaiting replication failed for {}".format(client_conn))
        self.hook_logger.info("WaitForReplication took %0.4f seconds", time.time() - start_time)
