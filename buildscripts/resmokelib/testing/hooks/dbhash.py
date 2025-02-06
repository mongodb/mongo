"""Test hook for verifying data consistency across a replica set."""

import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class CheckReplDBHash(jsfile.PerClusterDataConsistencyHook):
    """Check if the dbhashes match.

    This includes dbhashes for all non-local databases and non-replicated system collections that
    match on the primary and secondaries.
    """

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckReplDBHash."""
        description = "Check dbhashes of all replica set or master/slave members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_dbhash.js")
        jsfile.JSHook.__init__(
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
