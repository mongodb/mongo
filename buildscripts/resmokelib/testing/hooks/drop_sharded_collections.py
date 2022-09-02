"""Hook for cleaning up sharded collections created during tests."""

import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class DropShardedCollections(jsfile.JSHook):
    """Drops all sharded collections.

    With the exception of internal collections like config.system.sessions.
    """

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """."""
        description = "Drop all sharded collections"
        js_filename = os.path.join("jstests", "hooks", "drop_sharded_collections.js")
        jsfile.JSHook.__init__(self, hook_logger, fixture, js_filename, description,
                               shell_options=shell_options)
