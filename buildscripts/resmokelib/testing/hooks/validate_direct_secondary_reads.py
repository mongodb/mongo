import os.path

from buildscripts.resmokelib.testing.hooks import jsfile


class ValidateDirectSecondaryReads(jsfile.PerClusterDataConsistencyHook):
    """Only supported in suites that use ReplicaSetFixture.

    To be used with set_read_preference_secondary.js and implicit_enable_profiler.js in suites
    that read directly from secondaries in a replica set. Check the profiler collections of all
    databases at the end of the suite to verify that each secondary only ran the read commands it
    got directly from the shell.
    """

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize ValidateDirectSecondaryReads."""
        description = "Validate direct secondary reads"
        js_filename = os.path.join("jstests", "hooks", "run_validate_direct_secondary_reads.js")
        jsfile.JSHook.__init__(
            self, hook_logger, fixture, js_filename, description, shell_options=shell_options
        )
