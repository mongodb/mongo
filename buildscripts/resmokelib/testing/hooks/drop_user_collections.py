"""Hook for cleaning up user collections created during tests."""

import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class DropUserCollections(jsfile.JSHook):
    """Drops all user collections."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """."""
        description = "Drop all user collections"
        js_filename = os.path.join("jstests", "hooks", "drop_user_collections.js")
        jsfile.JSHook.__init__(
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
