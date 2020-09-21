"""Test hook for resetting the server in a sane state.

The main use case is to ensure that other hooks that will run against the server will not
encounter unexpected failures.
"""

import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class FuzzerRestoreSettings(jsfile.JSHook):
    """Cleans up unwanted changes from fuzzer."""

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Run fuzzer cleanup."""
        description = "Clean up unwanted changes from fuzzer"
        js_filename = os.path.join("jstests", "hooks", "run_fuzzer_restore_settings.js")
        shell_options = shell_options or {}
        # If the fuzz test by chance enabled the requireApiVersion parameter, permit the shell
        # to connect anyway and disable requireApiVersion.
        shell_options.setdefault("apiVersion", 1)
        jsfile.JSHook.__init__(self, hook_logger, fixture, js_filename, description,
                               shell_options=shell_options)
