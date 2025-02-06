"""Test hook for verifying members of a replica set have consistent config.system.preimages."""

import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class CheckReplPreImagesConsistency(jsfile.PerClusterDataConsistencyHook):
    """Check that config.system.preimages is consistent between the primary and secondaries."""

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckReplPreImagesConsistency."""
        description = "Check pre-images of all replica set members"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_pre_images.js")
        jsfile.JSHook.__init__(
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
