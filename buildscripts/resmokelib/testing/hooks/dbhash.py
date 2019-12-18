"""Test hook for verifying data consistency across a replica set."""

import os.path

from . import jsfile


class CheckReplDBHash(jsfile.DataConsistencyHook):
    """Check if the dbhashes match.

    This includes dbhashes for all non-local databases and non-replicated system collections that
    match on the primary and secondaries.
    """

    def __init__(  # pylint: disable=super-init-not-called
            self, hook_logger, fixture, shell_options=None):
        """Initialize CheckReplDBHash."""
        description = "Check dbhashes of all replica set or master/slave members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_dbhash.js")
        jsfile.JSHook.__init__(  # pylint: disable=non-parent-init-called
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options)
