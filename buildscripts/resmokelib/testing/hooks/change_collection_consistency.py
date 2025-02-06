"""Test hook for verifying members of a replica set have consistent config.system.change_collection for each tenant."""

import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class CheckReplChangeCollectionConsistency(jsfile.PerClusterDataConsistencyHook):
    """Check that config.system.change_collection is consistent between the primary and secondaries for all tenants."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckReplChangeCollectionConsistency."""
        description = "Check change_collection(s) of all replica set members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_change_collection.js")
        jsfile.JSHook.__init__(
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
