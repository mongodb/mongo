"""Hook for cleaning up sharded collections created during tests."""

from __future__ import absolute_import

import os.path

from . import jsfile


class DropShardedCollections(jsfile.JSHook):
    """Drops all sharded collections.

    With the exception of internal collections like config.system.sessions.
    """

    def __init__(  # pylint: disable=super-init-not-called
            self, hook_logger, fixture, shell_options=None):
        """."""
        description = "Drop all sharded collections"
        js_filename = os.path.join("jstests", "hooks", "drop_sharded_collections.js")
        jsfile.JSHook.__init__(  # pylint: disable=non-parent-init-called
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options)
